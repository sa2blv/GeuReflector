/**
@file   AnnounceManager.cpp
@brief  Queues and plays audio announcements to talk groups and single nodes
@date   2026-11-08

\verbatim
This file is part of GeuReflector / SvxLink.
Licensed under the GNU General Public License v2 or later.
\endverbatim
*/

#include <algorithm>
#include <cctype>
#include <istream>
#include <iterator>
#include <ostream>
#include <sstream>

#include <AsyncConfig.h>
#include <Log.h>

#include "AnnouncePlayer.h"
#include "AnnounceManager.h"


namespace {
  std::string upper(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
  }

  long msSince(const struct timespec& then)
  {
    if ((then.tv_sec == 0) && (then.tv_nsec == 0))
    {
      return 1L << 30;   // never seen: "long ago"
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - then.tv_sec) * 1000L +
           (now.tv_nsec - then.tv_nsec) / 1000000L;
  }

  bool parseToMesh(const std::string& v, AnnounceScope& out)
  {
    if (v.empty()) return false;
    if (v == "0") { out = AnnounceScope::LOCAL; return true; }
    if (v == "1") { out = AnnounceScope::BOTH;  return true; }
    return parseAnnounceScope(v, out);
  }

  // Config name of a feedback kind: <NAME>_CLIP, <NAME>_MAX, <NAME>_WINDOW
  const char* feedbackName(AnnounceManager::Feedback f)
  {
    switch (f)
    {
      case AnnounceManager::Feedback::QSY_BLOCKED:  return "QSY_BLOCKED";
      case AnnounceManager::Feedback::QSY_DISABLED: return "QSY_DISABLED";
      case AnnounceManager::Feedback::NODE_BLOCKED: return "NODE_BLOCKED";
      case AnnounceManager::Feedback::ON_TALK:      return "ON_TALK";
    }
    return "";
  }

  std::string feedbackKey(AnnounceManager::Feedback f)
  {
    return std::string(feedbackName(f)) + "_CLIP";
  }

  bool toUnsigned(const std::string& s, unsigned& out)
  {
    if (s.empty()) return false;
    try
    {
      out = static_cast<unsigned>(std::stoul(s));
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  const unsigned POLL_MS = 100;
}


const char* announceScopeStr(AnnounceScope s)
{
  switch (s)
  {
    case AnnounceScope::LOCAL: return "LOCAL";
    case AnnounceScope::TRUNK: return "TRUNK";
    case AnnounceScope::BOTH:  return "BOTH";
  }
  return "?";
} /* announceScopeStr */


bool parseAnnounceScope(const std::string& s, AnnounceScope& out)
{
  const std::string u = upper(s);
  if (u == "LOCAL")                   { out = AnnounceScope::LOCAL; return true; }
  if ((u == "TRUNK") || (u == "MESH")) { out = AnnounceScope::TRUNK; return true; }
  if (u == "BOTH")                    { out = AnnounceScope::BOTH;  return true; }
  return false;
} /* parseAnnounceScope */


AnnounceManager::AnnounceManager(const Async::Config& cfg,
                                 const std::string& section,
                                 const std::string& codec)
  : m_cfg(cfg), m_section(section), m_codec(codec),
    m_poll_timer(POLL_MS, Async::Timer::TYPE_PERIODIC, false)
{
  std::string v;
  m_cfg.getValue(m_section, "DIR", m_dir);
  while ((m_dir.size() > 1) && (m_dir.back() == '/')) m_dir.pop_back();
  if (m_cfg.getValue(m_section, "CALLSIGN", v) && !v.empty()) m_callsign = v;
  AnnounceScope sc;
  if (m_cfg.getValue(m_section, "TO_MESH", v) && parseToMesh(v, sc))
  {
    m_to_mesh = (sc != AnnounceScope::LOCAL);
  }
  auto getU = [&](const char* tag, unsigned& dst, unsigned lo, unsigned hi)
  {
    std::string s;
    if (m_cfg.getValue(m_section, tag, s) && !s.empty())
    {
      try
      {
        unsigned long n = std::stoul(s);
        dst = static_cast<unsigned>(std::max<unsigned long>(lo,
                                    std::min<unsigned long>(hi, n)));
      }
      catch (...)
      {
        geulog::warn("core", "[", m_section, "] ", tag, "=", s,
                     " is not a number, using ", dst);
      }
    }
  };
  getU("FRAME_MS",              m_frame_ms,          10, 60);
  getU("FEEDBACK_MIN_INTERVAL", m_feedback_interval,  0, 86400);
  getU("FEEDBACK_MAX_REPEAT",   m_feedback_max,       0, 1000);
  getU("FEEDBACK_RESET",        m_feedback_window,    1, 86400);  // old name
  getU("FEEDBACK_WINDOW",       m_feedback_window,    1, 86400);
  getU("ON_TALK_QSY_DELAY_MS",  m_ontalk_qsy_delay_ms, 0, 10000);
  getU("NODE_IDLE_MS",          m_node_idle_ms,       0, 10000);
  getU("NODE_MAX_WAIT",         m_node_max_wait,      0, 600);
  getU("TG_MAX_WAIT",           m_tg_max_wait,        0, 3600);
  getU("MAX_QUEUE",             m_max_queue,          1, 100);
  getU("MAX_LENGTH",            m_max_length,         1, 3600);
  if (m_cfg.getValue(m_section, "GAIN_DB", v) && !v.empty())
  {
    try { m_gain_db = std::stof(v); } catch (...) {}
  }

  if (upper(m_codec) != "OPUS")
  {
    geulog::warn("core", "[", m_section, "] Announcements are only tested "
                 "with the OPUS codec, the reflector uses ", m_codec);
  }
  geulog::info("core", "Announcements enabled from [", m_section, "]: dir='",
               m_dir, "' callsign=", m_callsign, " default scope=",
               (m_to_mesh ? "BOTH" : "LOCAL"), " codec=", m_codec);

  m_poll_timer.expired.connect(sigc::mem_fun(*this, &AnnounceManager::onPoll));

  // ON_TALK settings are looked up per audio packet, so they are cached and
  // the cache is dropped whenever the configuration changes (PTY CFG etc.).
  const_cast<Async::Config&>(m_cfg).valueUpdated.connect(
      [this](const std::string&, const std::string&)
      {
        m_ontalk_cache.clear();
      });
} /* AnnounceManager::AnnounceManager */


AnnounceManager::~AnnounceManager(void)
{
  m_poll_timer.setEnable(false);
  for (auto& kv : m_tgs)   { if (kv.second.player) kv.second.player->stop(); }
  for (auto& kv : m_nodes) { if (kv.second.player) kv.second.player->stop(); }
} /* AnnounceManager::~AnnounceManager */


/* --------------------------------------------------------------------- */

std::string AnnounceManager::tgValue(uint32_t tg, const std::string& tag,
                                     const std::string& def) const
{
  std::string v;
  if ((tg > 0) &&
      m_cfg.getValue("TG#" + std::to_string(tg), "ANNOUNCE_" + tag, v) &&
      !v.empty())
  {
    return v;
  }
  if (m_cfg.getValue(m_section, tag, v) && !v.empty())
  {
    return v;
  }
  return def;
} /* AnnounceManager::tgValue */


std::string AnnounceManager::callsignFor(uint32_t tg) const
{
  return tgValue(tg, "CALLSIGN", m_callsign);
} /* AnnounceManager::callsignFor */


AnnounceScope AnnounceManager::defaultScope(uint32_t tg) const
{
  auto it = m_scope_override.find(tg);
  if (it != m_scope_override.end())
  {
    return it->second;
  }
  std::string v;
  AnnounceScope sc;
  if (m_cfg.getValue("TG#" + std::to_string(tg), "ANNOUNCE_TO_MESH", v) &&
      parseToMesh(v, sc))
  {
    return sc;
  }
  return m_to_mesh ? AnnounceScope::BOTH : AnnounceScope::LOCAL;
} /* AnnounceManager::defaultScope */


bool AnnounceManager::resolvePath(const std::string& file, std::string& path,
                                  std::string& err) const
{
  if (file.empty())
  {
    err = "no file given";
    return false;
  }
  // Refuse any ".." path component so the PTY cannot escape DIR.
  std::istringstream comps(file);
  std::string c;
  while (std::getline(comps, c, '/'))
  {
    if (c == "..")
    {
      err = "'..' is not allowed in announcement file names";
      return false;
    }
  }
  if (file[0] == '/')
  {
    if (!m_dir.empty())
    {
      err = "absolute paths are not allowed when DIR is set";
      return false;
    }
    path = file;
  }
  else
  {
    path = (m_dir.empty() ? std::string(".") : m_dir) + "/" + file;
  }
  // "info" -> "info.wav"
  std::string::size_type slash = path.rfind('/');
  if (path.find('.', (slash == std::string::npos) ? 0 : slash) ==
      std::string::npos)
  {
    path += ".wav";
  }
  return true;
} /* AnnounceManager::resolvePath */


std::unique_ptr<AnnouncePlayer> AnnounceManager::makePlayer(
    const std::string& file, std::string& err) const
{
  std::string path;
  if (!resolvePath(file, path, err))
  {
    return nullptr;
  }
  std::vector<float> pcm;
  if (!AnnouncePlayer::loadWav(path, m_gain_db, m_max_length, pcm, err))
  {
    return nullptr;
  }
  std::unique_ptr<AnnouncePlayer> p(
      new AnnouncePlayer(m_codec, m_frame_ms, std::move(pcm)));
  if (!p->isValid())
  {
    err = "could not create a " + m_codec + " encoder";
    return nullptr;
  }
  return p;
} /* AnnounceManager::makePlayer */


/* --------------------------------------------------------------------- */

bool AnnounceManager::playTg(uint32_t tg, const std::string& file,
                             const AnnounceScope* scope, std::string& err)
{
  if (tg == 0)
  {
    err = "TG #0 is not a talk group";
    return false;
  }
  if (!makePlayer(file, err))   // validate now so the PTY gets the error
  {
    return false;
  }
  TgState& st = m_tgs[tg];
  if (st.queue.size() >= m_max_queue)
  {
    err = "queue for TG #" + std::to_string(tg) + " is full (MAX_QUEUE)";
    return false;
  }
  Item it;
  it.file   = file;
  it.scope  = (scope != nullptr) ? *scope : defaultScope(tg);
  it.queued = time(nullptr);
  st.queue.push_back(it);
  geulog::info("core", "Announce: queued '", file, "' on TG #", tg,
               " scope=", announceScopeStr(it.scope));
  updatePollTimer();
  onPoll(nullptr);   // start right away if the TG is free
  return true;
} /* AnnounceManager::playTg */


bool AnnounceManager::playNode(uint32_t client_id, const std::string& file,
                               std::string& err, int kind)
{
  if (!makePlayer(file, err))
  {
    return false;
  }
  const bool is_new = (m_nodes.find(client_id) == m_nodes.end());
  NodeState& st = m_nodes[client_id];
  if (is_new)
  {
    // Treat "now" as the last audio so the clip waits NODE_IDLE_MS: the
    // request usually comes while the node is still transmitting.
    clock_gettime(CLOCK_MONOTONIC, &st.last_audio);
  }
  if (st.queue.size() >= m_max_queue)
  {
    err = "queue for the node is full (MAX_QUEUE)";
    return false;
  }
  Item it;
  it.file   = file;
  it.queued = time(nullptr);
  it.kind   = kind;
  st.queue.push_back(it);
  geulog::debug("core", "Announce: queued '", file, "' for client ",
                client_id);
  updatePollTimer();
  return true;
} /* AnnounceManager::playNode */


unsigned AnnounceManager::feedbackMax(uint32_t tg, Feedback kind) const
{
  unsigned v = m_feedback_max;
  toUnsigned(tgValue(tg, std::string(feedbackName(kind)) + "_MAX", ""), v);
  return v;
} /* AnnounceManager::feedbackMax */


unsigned AnnounceManager::feedbackWindow(uint32_t tg, Feedback kind) const
{
  unsigned v = m_feedback_window;
  toUnsigned(tgValue(tg, std::string(feedbackName(kind)) + "_WINDOW", ""),
             v);
  return std::max(1u, v);
} /* AnnounceManager::feedbackWindow */


const AnnounceManager::OnTalkCfg& AnnounceManager::onTalkCfg(uint32_t tg)
{
  auto it = m_ontalk_cache.find(tg);
  if (it != m_ontalk_cache.end())
  {
    return it->second;
  }
  OnTalkCfg c;
  if (tg > 0)
  {
    // Only per TG: a default in [ANNOUNCE] would fire on every TG.
    const std::string sec = "TG#" + std::to_string(tg);
    std::string v;
    c.enabled = m_cfg.getValue(sec, "ANNOUNCE_ON_TALK_CLIP", v) && !v.empty();
    v.clear();
    c.priv = c.enabled &&
             m_cfg.getValue(sec, "ANNOUNCE_ON_TALK_PRIVATE", v) &&
             (v == "1" || upper(v) == "YES" || upper(v) == "TRUE");
  }
  return m_ontalk_cache[tg] = c;
} /* AnnounceManager::onTalkCfg */


bool AnnounceManager::hasOnTalk(uint32_t tg)
{
  return onTalkCfg(tg).enabled;
} /* AnnounceManager::hasOnTalk */


bool AnnounceManager::onTalkPrivate(uint32_t tg)
{
  return onTalkCfg(tg).priv;
} /* AnnounceManager::onTalkPrivate */


void AnnounceManager::feedback(uint32_t client_id, uint32_t tg, Feedback kind)
{
  if ((kind == Feedback::ON_TALK) &&
      (m_ontalk_aborted.find(client_id) != m_ontalk_aborted.end()))
  {
    return;   // the press that cut the clip off does not start it again
  }
  std::string file;
  if (kind == Feedback::ON_TALK)
  {
    m_cfg.getValue("TG#" + std::to_string(tg), "ANNOUNCE_ON_TALK_CLIP", file);
  }
  else
  {
    file = tgValue(tg, feedbackKey(kind), "");
  }
  if (file.empty() && (kind == Feedback::QSY_DISABLED))
  {
    file = tgValue(tg, feedbackKey(Feedback::QSY_BLOCKED), "");
  }
  if (file.empty())
  {
    return;
  }
  const int k = static_cast<int>(kind);
  const time_t now = time(nullptr);
  FeedbackState& fs = m_feedback[std::make_pair(client_id, k)];
  if (fs.tg != tg)
  {
    fs = FeedbackState();   // another TG: start counting from zero
  }
  fs.tg = tg;
  fs.window = feedbackWindow(tg, kind);
  while (!fs.plays.empty() &&
         (now - fs.plays.front() >= static_cast<time_t>(fs.window)))
  {
    fs.plays.pop_front();   // sliding window
    fs.capped_logged = false;
  }

  // One clip per attempt: the packets of one PTT press (or a repeated
  // request while the clip is still waiting/playing) do not queue more.
  auto nit = m_nodes.find(client_id);
  if (nit != m_nodes.end())
  {
    if (nit->second.player && (nit->second.playing_kind == k))
    {
      return;
    }
    for (const auto& item : nit->second.queue)
    {
      if (item.kind == k)
      {
        return;
      }
    }
  }

  const unsigned max = feedbackMax(tg, kind);
  if ((max > 0) && (fs.plays.size() >= max))
  {
    if (kind == Feedback::ON_TALK)
    {
      // Already heard it enough: still keep the info TG clear.
      scheduleOnTalkQsy(client_id, tg,
                        std::max(m_node_idle_ms, m_ontalk_qsy_delay_ms));
    }
    if (!fs.capped_logged)
    {
      fs.capped_logged = true;
      geulog::info("core", "Announce: ", feedbackKey(kind), " for client ",
                   client_id, " on TG #", tg, " played ", fs.plays.size(),
                   " times in ", fs.window, " s, muted until the window "
                   "has passed or the node changes TG");
    }
    return;
  }
  if ((m_feedback_interval > 0) && !fs.plays.empty() &&
      (now - fs.plays.back() < static_cast<time_t>(m_feedback_interval)))
  {
    return;
  }

  std::string err;
  if (!playNode(client_id, file, err, k))
  {
    geulog::warn("core", "Announce: feedback ", feedbackKey(kind),
                 " failed: ", err);
    return;
  }
  fs.plays.push_back(now);
} /* AnnounceManager::feedback */


void AnnounceManager::nodeTgChanged(uint32_t client_id)
{
  m_ontalk_aborted.erase(client_id);
  for (auto it = m_feedback.begin(); it != m_feedback.end(); )
  {
    it = (it->first.first == client_id) ? m_feedback.erase(it)
                                        : std::next(it);
  }
} /* AnnounceManager::nodeTgChanged */


void AnnounceManager::stopTg(uint32_t tg)
{
  auto it = m_tgs.find(tg);
  if (it == m_tgs.end())
  {
    return;
  }
  it->second.queue.clear();
  if (it->second.player)
  {
    finishTg(tg, true);
  }
} /* AnnounceManager::stopTg */


void AnnounceManager::stopNode(uint32_t client_id)
{
  auto it = m_nodes.find(client_id);
  if (it == m_nodes.end())
  {
    return;
  }
  it->second.queue.clear();
  if (it->second.player)
  {
    finishNode(client_id, /*allow_qsy=*/false);   // stopped by the operator
  }
} /* AnnounceManager::stopNode */


void AnnounceManager::nodeGone(uint32_t client_id)
{
  m_pending_qsy.erase(
      std::remove_if(m_pending_qsy.begin(), m_pending_qsy.end(),
                     [client_id](const PendingQsy& q)
                     { return q.cid == client_id; }),
      m_pending_qsy.end());
  nodeTgChanged(client_id);   // client ids are reused: forget the counters
  auto it = m_nodes.find(client_id);
  if (it == m_nodes.end())
  {
    return;
  }
  if (it->second.player)
  {
    it->second.player->stop();
    bury(it->second.player);
  }
  m_nodes.erase(it);
  updatePollTimer();
} /* AnnounceManager::nodeGone */


void AnnounceManager::nodeAudioSeen(uint32_t client_id)
{
  // A new PTT press (silence before it) re-arms ON_TALK after an abort.
  auto ait = m_ontalk_aborted.find(client_id);
  if (ait != m_ontalk_aborted.end())
  {
    if (msSince(ait->second) >= static_cast<long>(m_node_idle_ms))
    {
      m_ontalk_aborted.erase(ait);
    }
    else
    {
      clock_gettime(CLOCK_MONOTONIC, &ait->second);
    }
  }

  // A pending QSY waits until the node has been quiet for its delay.
  for (auto& q : m_pending_qsy)
  {
    if (q.cid == client_id)
    {
      clock_gettime(CLOCK_MONOTONIC, &q.since);
    }
  }

  auto it = m_nodes.find(client_id);
  if (it != m_nodes.end())
  {
    clock_gettime(CLOCK_MONOTONIC, &it->second.last_audio);
    it->second.wait_since = 0;
    // PTT while the info/help clip plays: the user wants it to stop.
    if (it->second.player &&
        (it->second.playing_kind == static_cast<int>(Feedback::ON_TALK)))
    {
      geulog::info("core", "Announce: client ", client_id,
                   " pressed PTT, stopping the ON_TALK clip");
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      m_ontalk_aborted[client_id] = now;
      finishNode(client_id);
    }
  }
} /* AnnounceManager::nodeAudioSeen */


bool AnnounceManager::ownsTg(uint32_t tg) const
{
  auto it = m_tgs.find(tg);
  return (it != m_tgs.end()) && it->second.player && it->second.hdx;
} /* AnnounceManager::ownsTg */


bool AnnounceManager::isPlayingTg(uint32_t tg) const
{
  auto it = m_tgs.find(tg);
  return (it != m_tgs.end()) && it->second.player;
} /* AnnounceManager::isPlayingTg */


AnnounceScope AnnounceManager::playingScope(uint32_t tg) const
{
  auto it = m_tgs.find(tg);
  return (it != m_tgs.end()) ? it->second.scope : AnnounceScope::LOCAL;
} /* AnnounceManager::playingScope */


/* --------------------------------------------------------------------- */

void AnnounceManager::startTg(uint32_t tg, TgState& st)
{
  Item item = st.queue.front();
  st.queue.pop_front();

  std::string err;
  std::unique_ptr<AnnouncePlayer> p = makePlayer(item.file, err);
  if (!p)
  {
    geulog::warn("core", "Announce: TG #", tg, ": ", err);
    return;
  }
  const bool fdx = tgFullDuplex ? tgFullDuplex(tg) : false;
  st.scope    = item.scope;
  st.callsign = callsignFor(tg);
  // Exclusive = we hold the TG alone and yield if real traffic appears.
  // Full duplex LOCAL/BOTH is mixed with the other talkers instead.
  st.hdx      = !fdx || (item.scope == AnnounceScope::TRUNK);
  st.player   = std::move(p);

  st.player->frameReady.connect([this, tg](const std::vector<uint8_t>& f)
  {
    auto it = m_tgs.find(tg);
    if ((it == m_tgs.end()) || !it->second.player) return;
    if (it->second.hdx && tgBusy && tgBusy(tg))
    {
      geulog::info("core", "Announce: TG #", tg,
                   " taken by other traffic, aborting announcement");
      finishTg(tg, true);
      return;
    }
    tgFrame(tg, f, it->second.scope);
  });
  st.player->finished.connect([this, tg]() { finishTg(tg, false); });

  geulog::info("core", "Announce: playing '", item.file, "' (",
               st.player->lengthMs(), " ms) on TG #", tg, " as ",
               st.callsign, " scope=", announceScopeStr(st.scope),
               fdx ? " (full duplex)" : "");
  tgStart(tg, st.callsign, st.scope);
  st.player->start();
} /* AnnounceManager::startTg */


void AnnounceManager::finishTg(uint32_t tg, bool aborted)
{
  auto it = m_tgs.find(tg);
  if ((it == m_tgs.end()) || !it->second.player)
  {
    return;
  }
  TgState& st = it->second;
  st.player->stop();
  bury(st.player);
  geulog::info("core", "Announce: ", aborted ? "aborted" : "done",
               " on TG #", tg);
  tgStop(tg, st.callsign, st.scope);
  updatePollTimer();
} /* AnnounceManager::finishTg */


void AnnounceManager::startNode(uint32_t cid, NodeState& st)
{
  Item item = st.queue.front();
  st.queue.pop_front();

  std::string err;
  std::unique_ptr<AnnouncePlayer> p = makePlayer(item.file, err);
  if (!p)
  {
    geulog::warn("core", "Announce: client ", cid, ": ", err);
    return;
  }
  st.player   = std::move(p);
  st.callsign = callsignFor(st.tg);
  st.playing_kind = item.kind;

  st.player->frameReady.connect([this, cid](const std::vector<uint8_t>& f)
  {
    nodeFrame(cid, f);
  });
  st.player->finished.connect([this, cid]() { finishNode(cid); });

  geulog::info("core", "Announce: playing '", item.file, "' to client ",
               cid, " on TG #", st.tg);
  nodeStart(cid, st.tg, st.callsign);
  st.player->start();
} /* AnnounceManager::startNode */


void AnnounceManager::finishNode(uint32_t cid, bool allow_qsy)
{
  auto it = m_nodes.find(cid);
  if ((it == m_nodes.end()) || !it->second.player)
  {
    return;
  }
  NodeState& st = it->second;
  const int kind = st.playing_kind;
  const uint32_t tg = st.tg;
  st.player->stop();
  bury(st.player);
  st.wait_since = 0;
  st.playing_kind = -1;
  nodeStop(cid, tg, st.callsign);
  if (allow_qsy && (kind == static_cast<int>(Feedback::ON_TALK)))
  {
    // After a PTT abort the node is still transmitting: the delay restarts
    // with each packet (nodeAudioSeen), so the QSY comes once it is quiet.
    scheduleOnTalkQsy(cid, tg, std::max(m_node_idle_ms,
                                        m_ontalk_qsy_delay_ms));
  }
  updatePollTimer();
} /* AnnounceManager::finishNode */


void AnnounceManager::scheduleOnTalkQsy(uint32_t cid, uint32_t from_tg,
                                        unsigned delay_ms)
{
  std::string v;
  unsigned to = 0;
  if (!m_cfg.getValue("TG#" + std::to_string(from_tg), "ANNOUNCE_ON_TALK_QSY",
                      v) || !toUnsigned(v, to) || (to == from_tg))
  {
    return;   // not configured: the node stays on the TG
  }
  for (const auto& q : m_pending_qsy)
  {
    if (q.cid == cid)
    {
      return;
    }
  }
  PendingQsy q;
  q.cid = cid;
  q.from = from_tg;
  q.to = to;
  q.delay_ms = delay_ms;
  clock_gettime(CLOCK_MONOTONIC, &q.since);
  m_pending_qsy.push_back(q);
  updatePollTimer();
} /* AnnounceManager::scheduleOnTalkQsy */


void AnnounceManager::bury(std::unique_ptr<AnnouncePlayer>& p)
{
  // A player may be finishing from inside its own timer callback, so it is
  // deleted on the next poll tick rather than right here.
  m_graveyard.push_back(std::move(p));
} /* AnnounceManager::bury */


void AnnounceManager::updatePollTimer(void)
{
  bool need = !m_graveyard.empty() || !m_pending_qsy.empty();
  for (const auto& kv : m_tgs)
  {
    need = need || kv.second.player || !kv.second.queue.empty();
  }
  for (const auto& kv : m_nodes)
  {
    need = need || kv.second.player || !kv.second.queue.empty();
  }
  m_poll_timer.setEnable(need);
} /* AnnounceManager::updatePollTimer */


void AnnounceManager::onPoll(Async::Timer* t)
{
  if (t != nullptr)
  {
    m_graveyard.clear();   // only from the timer: never inside a callback
  }
  const time_t now = time(nullptr);

  for (auto it = m_tgs.begin(); it != m_tgs.end(); )
  {
    const uint32_t tg = it->first;
    TgState& st = it->second;
    while (!st.player && !st.queue.empty())
    {
      const Item& item = st.queue.front();
      if ((m_tg_max_wait > 0) &&
          (now - item.queued > static_cast<time_t>(m_tg_max_wait)))
      {
        geulog::warn("core", "Announce: TG #", tg, " busy for more than ",
                     m_tg_max_wait, " s, dropping '", item.file, "'");
        st.queue.pop_front();
        continue;
      }
      const bool fdx = tgFullDuplex ? tgFullDuplex(tg) : false;
      const bool exclusive = !fdx || (item.scope == AnnounceScope::TRUNK);
      if (exclusive && tgBusy && tgBusy(tg))
      {
        break;   // wait for the TG to become free
      }
      startTg(tg, st);
    }
    if (!st.player && st.queue.empty())
    {
      it = m_tgs.erase(it);
    }
    else
    {
      ++it;
    }
  }

  for (auto it = m_nodes.begin(); it != m_nodes.end(); )
  {
    const uint32_t cid = it->first;
    NodeState& st = it->second;
    if (!st.player && !st.queue.empty())
    {
      const int64_t tg = nodeTg ? nodeTg(cid) : 0;
      if (tg < 0)
      {
        st.queue.clear();   // node gone
      }
      else if ((m_tg_max_wait > 0) &&
               (now - st.queue.front().queued >
                static_cast<time_t>(m_tg_max_wait)))
      {
        st.queue.pop_front();
      }
      else if (msSince(st.last_audio) >= static_cast<long>(m_node_idle_ms))
      {
        // The node has released PTT. Prefer a quiet TG so the clip does
        // not cut into a QSO, but never wait longer than NODE_MAX_WAIT.
        if (st.wait_since == 0) st.wait_since = now;
        st.tg = static_cast<uint32_t>(tg);
        const bool busy = (tgBusy && tgBusy(st.tg)) || isPlayingTg(st.tg);
        if (!busy ||
            (now - st.wait_since >= static_cast<time_t>(m_node_max_wait)))
        {
          startNode(cid, st);
        }
      }
    }
    if (!st.player && st.queue.empty())
    {
      it = m_nodes.erase(it);
    }
    else
    {
      ++it;
    }
  }

  for (auto it = m_pending_qsy.begin(); it != m_pending_qsy.end(); )
  {
    if (msSince(it->since) < static_cast<long>(it->delay_ms))
    {
      ++it;
      continue;
    }
    const PendingQsy q = *it;
    it = m_pending_qsy.erase(it);
    // Only if the node is still on the info TG and not busy with a clip
    auto nit = m_nodes.find(q.cid);
    if ((nodeTg ? nodeTg(q.cid) : -1) == static_cast<int64_t>(q.from) &&
        ((nit == m_nodes.end()) || !nit->second.player))
    {
      geulog::info("core", "Announce: QSY client ", q.cid, " from TG #",
                   q.from, " to TG #", q.to, " (ANNOUNCE_ON_TALK_QSY)");
      nodeQsy(q.cid, q.from, q.to);
    }
  }

  for (auto it = m_ontalk_aborted.begin(); it != m_ontalk_aborted.end(); )
  {
    it = (msSince(it->second) >= static_cast<long>(m_node_idle_ms))
           ? m_ontalk_aborted.erase(it) : std::next(it);
  }

  // Forget feedback counters whose window has passed anyway
  for (auto it = m_feedback.begin(); it != m_feedback.end(); )
  {
    if (it->second.plays.empty() ||
        (now - it->second.plays.back() >
         static_cast<time_t>(it->second.window)))
    {
      it = m_feedback.erase(it);
    }
    else
    {
      ++it;
    }
  }

  updatePollTimer();
} /* AnnounceManager::onPoll */


/* --------------------------------------------------------------------- */

const char* AnnounceManager::ptyUsage(void)
{
  return "ANNOUNCE PLAY TG <tg> <file> [LOCAL|TRUNK|BOTH]\n"
         "ANNOUNCE PLAY NODE <callsign> <file>\n"
         "ANNOUNCE STOP TG <tg> | STOP NODE <callsign> | STOP ALL\n"
         "ANNOUNCE SCOPE <tg> <LOCAL|TRUNK|BOTH|DEFAULT>\n"
         "ANNOUNCE LIST\n";
} /* AnnounceManager::ptyUsage */


void AnnounceManager::handlePtyCommand(std::istream& args, std::ostream& out,
                                       std::ostream& err)
{
  std::string sub;
  args >> sub;
  sub = upper(sub);

  if (sub == "PLAY")
  {
    std::string what, target, file, scope_str;
    args >> what >> target >> file >> scope_str;
    what = upper(what);
    std::string e;
    if (what == "TG")
    {
      uint32_t tg = 0;
      try { tg = static_cast<uint32_t>(std::stoul(target)); } catch (...) {}
      AnnounceScope scope;
      if (!scope_str.empty() && !parseAnnounceScope(scope_str, scope))
      {
        err << "Invalid scope '" << scope_str << "' (LOCAL|TRUNK|BOTH)";
        return;
      }
      if (!playTg(tg, file, scope_str.empty() ? nullptr : &scope, e))
      {
        err << "ANNOUNCE PLAY TG: " << e;
        return;
      }
      out << "ANNOUNCE: queued " << file << " on TG #" << tg << " scope="
          << announceScopeStr(scope_str.empty() ? defaultScope(tg) : scope)
          << "\n";
    }
    else if (what == "NODE")
    {
      uint32_t cid = 0;
      if (!findNode || !findNode(target, cid))
      {
        err << "ANNOUNCE PLAY NODE: node " << target << " is not connected";
        return;
      }
      if (!playNode(cid, file, e))
      {
        err << "ANNOUNCE PLAY NODE: " << e;
        return;
      }
      updatePollTimer();
      out << "ANNOUNCE: queued " << file << " for " << target << "\n";
    }
    else
    {
      err << "Usage:\n" << ptyUsage();
    }
  }
  else if (sub == "STOP")
  {
    std::string what, target;
    args >> what >> target;
    what = upper(what);
    if (what == "TG")
    {
      uint32_t tg = 0;
      try { tg = static_cast<uint32_t>(std::stoul(target)); } catch (...) {}
      stopTg(tg);
    }
    else if (what == "NODE")
    {
      uint32_t cid = 0;
      if (!findNode || !findNode(target, cid))
      {
        err << "ANNOUNCE STOP NODE: node " << target << " is not connected";
        return;
      }
      stopNode(cid);
    }
    else if (what == "ALL")
    {
      std::vector<uint32_t> tgs, cids;
      for (const auto& kv : m_tgs)   tgs.push_back(kv.first);
      for (const auto& kv : m_nodes) cids.push_back(kv.first);
      for (auto tg : tgs)   stopTg(tg);
      for (auto cid : cids) stopNode(cid);
    }
    else
    {
      err << "Usage:\n" << ptyUsage();
    }
  }
  else if (sub == "SCOPE")
  {
    std::string target, scope_str;
    args >> target >> scope_str;
    uint32_t tg = 0;
    try { tg = static_cast<uint32_t>(std::stoul(target)); } catch (...) {}
    if (tg == 0)
    {
      err << "ANNOUNCE SCOPE: invalid TG '" << target << "'";
      return;
    }
    if (upper(scope_str) == "DEFAULT")
    {
      m_scope_override.erase(tg);
    }
    else
    {
      AnnounceScope scope;
      if (!parseAnnounceScope(scope_str, scope))
      {
        err << "Invalid scope '" << scope_str
            << "' (LOCAL|TRUNK|BOTH|DEFAULT)";
        return;
      }
      m_scope_override[tg] = scope;
    }
    out << "ANNOUNCE: TG #" << tg << " default scope is now "
        << announceScopeStr(defaultScope(tg)) << "\n";
  }
  else if ((sub == "LIST") || (sub == "STATUS") || sub.empty())
  {
    out << "ANNOUNCE: callsign=" << m_callsign << " dir=" << m_dir
        << " default scope=" << (m_to_mesh ? "BOTH" : "LOCAL") << "\n";
    for (const auto& kv : m_scope_override)
    {
      out << "  scope override TG #" << kv.first << " = "
          << announceScopeStr(kv.second) << "\n";
    }
    for (const auto& kv : m_tgs)
    {
      out << "  TG #" << kv.first << ": "
          << (kv.second.player ? std::string("playing scope=") +
                announceScopeStr(kv.second.scope) : std::string("waiting"))
          << ", " << kv.second.queue.size() << " queued\n";
    }
    for (const auto& kv : m_nodes)
    {
      out << "  client " << kv.first << ": "
          << (kv.second.player ? "playing" : "waiting") << ", "
          << kv.second.queue.size() << " queued\n";
    }
  }
  else
  {
    err << "Usage:\n" << ptyUsage();
  }
} /* AnnounceManager::handlePtyCommand */
