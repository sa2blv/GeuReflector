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

  const char* feedbackKey(AnnounceManager::Feedback f)
  {
    switch (f)
    {
      case AnnounceManager::Feedback::QSY_BLOCKED:  return "QSY_BLOCKED_CLIP";
      case AnnounceManager::Feedback::QSY_DISABLED: return "QSY_DISABLED_CLIP";
      case AnnounceManager::Feedback::NODE_BLOCKED: return "NODE_BLOCKED_CLIP";
    }
    return "";
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
                               std::string& err)
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
  st.queue.push_back(it);
  geulog::debug("core", "Announce: queued '", file, "' for client ",
                client_id);
  updatePollTimer();
  return true;
} /* AnnounceManager::playNode */


void AnnounceManager::feedback(uint32_t client_id, uint32_t tg, Feedback kind)
{
  std::string file = tgValue(tg, feedbackKey(kind), "");
  if (file.empty() && (kind == Feedback::QSY_DISABLED))
  {
    file = tgValue(tg, feedbackKey(Feedback::QSY_BLOCKED), "");
  }
  if (file.empty())
  {
    return;
  }
  const auto key = std::make_pair(client_id, static_cast<int>(kind));
  const time_t now = time(nullptr);
  auto lit = m_last_feedback.find(key);
  if ((lit != m_last_feedback.end()) &&
      (now - lit->second < static_cast<time_t>(m_feedback_interval)))
  {
    return;
  }
  m_last_feedback[key] = now;

  std::string err;
  if (!playNode(client_id, file, err))
  {
    geulog::warn("core", "Announce: feedback ", feedbackKey(kind),
                 " failed: ", err);
  }
} /* AnnounceManager::feedback */


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
    finishNode(client_id);
  }
} /* AnnounceManager::stopNode */


void AnnounceManager::nodeGone(uint32_t client_id)
{
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
  for (auto lit = m_last_feedback.begin(); lit != m_last_feedback.end(); )
  {
    lit = (lit->first.first == client_id) ? m_last_feedback.erase(lit)
                                          : std::next(lit);
  }
  updatePollTimer();
} /* AnnounceManager::nodeGone */


void AnnounceManager::nodeAudioSeen(uint32_t client_id)
{
  auto it = m_nodes.find(client_id);
  if (it != m_nodes.end())
  {
    clock_gettime(CLOCK_MONOTONIC, &it->second.last_audio);
    it->second.wait_since = 0;
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


void AnnounceManager::finishNode(uint32_t cid)
{
  auto it = m_nodes.find(cid);
  if ((it == m_nodes.end()) || !it->second.player)
  {
    return;
  }
  NodeState& st = it->second;
  st.player->stop();
  bury(st.player);
  st.wait_since = 0;
  nodeStop(cid, st.tg, st.callsign);
  updatePollTimer();
} /* AnnounceManager::finishNode */


void AnnounceManager::bury(std::unique_ptr<AnnouncePlayer>& p)
{
  // A player may be finishing from inside its own timer callback, so it is
  // deleted on the next poll tick rather than right here.
  m_graveyard.push_back(std::move(p));
} /* AnnounceManager::bury */


void AnnounceManager::updatePollTimer(void)
{
  bool need = !m_graveyard.empty();
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

  // Forget old rate-limit entries
  for (auto it = m_last_feedback.begin(); it != m_last_feedback.end(); )
  {
    if (now - it->second > static_cast<time_t>(m_feedback_interval) + 60)
    {
      it = m_last_feedback.erase(it);
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
