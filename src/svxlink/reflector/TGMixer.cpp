/**
@file   TGMixer.cpp
@brief  Full-duplex per-talkgroup audio conference mixer for SvxReflector
@author SvxLink by Tobias Blomberg / SM0SVX; full-duplex extension
@date   2026-10-31

See TGMixer.h for the design overview.

\verbatim
This file is part of GeuReflector / SvxLink.
Licensed under the GNU General Public License v2 or later.
\endverbatim
*/


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <algorithm>
#include <set>
#include <cstring>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncAudioEncoder.h>
#include <AsyncAudioDecoder.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "TGMixer.h"


using namespace std;
using namespace Async;


/****************************************************************************
 *
 * Local helpers
 *
 ****************************************************************************/

namespace
{
  // Read an unsigned config value, per-TG ("TG#<tg>") overriding "GLOBAL",
  // clamped to [lo, hi]. Returns def if unset anywhere.
  unsigned cfgUnsigned(const Config& cfg, uint32_t tg, const string& key,
                       unsigned def, unsigned lo, unsigned hi)
  {
    ostringstream tgsec;
    tgsec << "TG#" << tg;
    unsigned val = def;
    string s;
    if (cfg.getValue(tgsec.str(), key, s) && !s.empty())
    {
      val = static_cast<unsigned>(strtoul(s.c_str(), nullptr, 10));
    }
    else if (cfg.getValue("GLOBAL", key, s) && !s.empty())
    {
      val = static_cast<unsigned>(strtoul(s.c_str(), nullptr, 10));
    }
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    return val;
  }
} /* anonymous namespace */


/****************************************************************************
 *
 * Nested class: SourceSink
 *
 ****************************************************************************/

// A leaf audio sink that captures decoded PCM from a source's Opus decoder
// into a ring buffer (deque). Oldest samples are dropped on overflow so a
// stuck source can never grow memory without bound.
class TGMixer::SourceSink : public Async::AudioSink
{
  public:
    SourceSink(std::deque<float>* ring, size_t cap)
      : m_ring(ring), m_cap(cap) {}

    virtual int writeSamples(const float *samples, int count) override
    {
      if (count <= 0)
      {
        return count;
      }
      for (int i = 0; i < count; ++i)
      {
        m_ring->push_back(samples[i]);
      }
      while (m_ring->size() > m_cap)
      {
        m_ring->pop_front();
      }
      return count;
    }

    virtual void flushSamples(void) override
    {
      // Leaf sink: acknowledge the flush so the source does not stall.
      sourceAllSamplesFlushed();
    }

  private:
    std::deque<float>* m_ring;
    size_t             m_cap;
}; /* class TGMixer::SourceSink */


/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

TGMixer::TGMixer(uint32_t tg, const Async::Config& cfg)
  : m_tg(tg),
    m_tick_timer(20, Async::Timer::TYPE_PERIODIC, false)
{
  m_frame_ms    = cfgUnsigned(cfg, tg, "MIX_FRAME_MS",    20,  5,  60);
  m_jitter_ms   = cfgUnsigned(cfg, tg, "MIX_JITTER_MS",   60,  0, 500);
  m_max_talkers = cfgUnsigned(cfg, tg, "MIX_MAX_TALKERS",  4,  1,  32);
  unsigned src_timeout_ms =
                  cfgUnsigned(cfg, tg, "MIX_SOURCE_TIMEOUT_MS",
                              10000, 1000, 600000);

  m_frame_size  = (SAMPLE_RATE * m_frame_ms) / 1000;
  if (m_frame_size == 0)
  {
    m_frame_size = SAMPLE_RATE / 50; // 20 ms fallback
  }
  m_prebuf_size = (SAMPLE_RATE * m_jitter_ms) / 1000;
  // At least one full frame of prebuffer unless jitter is explicitly 0.
  if ((m_jitter_ms > 0) && (m_prebuf_size < m_frame_size))
  {
    m_prebuf_size = m_frame_size;
  }
  // RESUME cushion after a mid-over gap: much smaller than the initial
  // prebuffer so audio comes back almost immediately instead of leaving a full
  // MIX_JITTER_MS (default 60 ms) silent hole. Only the first start uses the
  // full prebuffer; a resume just needs enough not to instantly re-underrun.
  // Default 20 ms (one frame); clamped to [1 frame, m_prebuf_size].
  unsigned reprebuf_target_ms =
      cfgUnsigned(cfg, tg, "MIX_REPREBUF_TARGET_MS", 20, 0, 500);
  m_reprebuf_target = (SAMPLE_RATE * reprebuf_target_ms) / 1000;
  if (m_reprebuf_target < m_frame_size) { m_reprebuf_target = m_frame_size; }
  if (m_reprebuf_target > m_prebuf_size) { m_reprebuf_target = m_prebuf_size; }
  // Hangover: keep a source "active" past its last audio so brief gaps between
  // packets do not flap the talker-start/stop or the mix-minus-self routing.
  // Configurable; default raised to 500 ms so a steady but jittery source
  // (a trunk/bridge, or a node whose packets arrive unevenly — duplex or not)
  // is not declared idle on short gaps. MIX_HANGOVER_MS overrides per-TG/GLOBAL.
  unsigned hangover_ms = cfgUnsigned(cfg, tg, "MIX_HANGOVER_MS", 500, 0, 5000);
  m_hangover_frames = (hangover_ms + m_frame_ms - 1) / m_frame_ms;
  // TalkerStop debounce: how long the TALKER state is held after the last active
  // frame, SEPARATE from (and >=) the mix hangover above. A continuous over has
  // natural gaps (network jitter, breaths, soft syllables); holding the talker
  // across them means ONE clean TalkerStart...TalkerStop per over instead of a
  // Start/Stop flap that clips word endings and trips "last talker audio
  // timeout" downstream. Mixing still fades on the short hangover, so other
  // listeners get no extra latency. Default 1500 ms; clamped to >= hangover.
  unsigned talker_hang_ms =
      cfgUnsigned(cfg, tg, "MIX_TALKER_HANG_MS", 1500, 0, 10000);
  m_talker_hang_frames = (talker_hang_ms + m_frame_ms - 1) / m_frame_ms;
  if (m_talker_hang_frames < m_hangover_frames)
  {
    m_talker_hang_frames = m_hangover_frames;
  }
  // The long hang above is for jittery MESH/trunk sources. A LOCAL client
  // (radio or app) delivers a clean packet stream, so it gets a much SHORTER
  // talker hang -- a physical radio keys on TalkerStart and unkeys on
  // TalkerStop, and a 1.5 s tail there feels "seg" (stays keyed long after the
  // audio ends). Default 300 ms.
  unsigned talker_hang_local_ms =
      cfgUnsigned(cfg, tg, "MIX_TALKER_HANG_LOCAL_MS", 300, 0, 10000);
  m_talker_hang_local_frames =
      (talker_hang_local_ms + m_frame_ms - 1) / m_frame_ms;
  if (m_talker_hang_local_frames < 1) { m_talker_hang_local_frames = 1; }
  // Only rebuild the jitter buffer (which injects a prebuffer of silence) after
  // a SUSTAINED outage, not on every late/partial frame — so transient network
  // jitter from a trunk source is absorbed instead of chopping the audio.
  unsigned reprebuf_ms = cfgUnsigned(cfg, tg, "MIX_REPREBUF_MS", 300, 20, 2000);
  m_reprebuf_frames = (reprebuf_ms + m_frame_ms - 1) / m_frame_ms;
  m_source_timeout_frames = (src_timeout_ms + m_frame_ms - 1) / m_frame_ms;
  // NOTE: no audio-level/VOX activity gate. svxref is COR-based: a source is
  // "active" while its carrier is open, i.e. while it actually streams audio
  // (TalkerStart..TalkerStop / packets present). Keying is never decided from
  // the audio level here. An ungated source that streams forever is a COR
  // problem on that source (e.g. an ASL bridge that must gate), not something
  // the reflector papers over.

  // How long a local source must be observed transmitting-while-hearing-others
  // before we treat it as full duplex (pure on-air learning, no node info).
  // A short sustain so a brief PTT-edge overlap does not misclassify a simplex
  // node. Default 200 ms.
  unsigned duplex_observe_ms =
      cfgUnsigned(cfg, tg, "MIX_DUPLEX_OBSERVE_MS", 200, 0, 5000);
  m_duplex_observe_frames = (duplex_observe_ms + m_frame_ms - 1) / m_frame_ms;
  if (m_duplex_observe_frames == 0) { m_duplex_observe_frames = 1; }

  // Window for the take-turns (half-duplex) test that can override a wrong
  // "full" freq hint back to simplex. Default 2 s.
  unsigned duplex_window_ms =
      cfgUnsigned(cfg, tg, "MIX_DUPLEX_WINDOW_MS", 2000, 200, 20000);
  m_duplex_window_frames = (duplex_window_ms + m_frame_ms - 1) / m_frame_ms;
  if (m_duplex_window_frames < 2) { m_duplex_window_frames = 2; }

  // NOTE: encoders (full, local, and per-source mix-minus-self) are NOT created
  // here. They are the expensive part (CPU + memory) and are only needed when
  // 2+ talkers actually overlap and the audio must be mixed. With <= 1 talker
  // the Reflector forwards the raw Opus (passthrough), so this mixer allocates
  // no encoders at all -- as lean as stock svxref for the common case. They are
  // created lazily by ensureMixEncoders()/ensureSourceEncoder() on entering mix
  // mode and freed by dropEncoders() on leaving it.

  m_tick_timer.setTimeout(static_cast<int>(m_frame_ms));
  m_tick_timer.expired.connect(sigc::mem_fun(*this, &TGMixer::onTick));
} /* TGMixer::TGMixer */


TGMixer::~TGMixer(void)
{
  for (auto& kv : m_sources)
  {
    destroySource(kv.second);
  }
  m_sources.clear();
  delete m_full_encoder;
  m_full_encoder = nullptr;
  delete m_local_encoder;
  m_local_encoder = nullptr;
} /* TGMixer::~TGMixer */


void TGMixer::addSource(const std::string& id, bool local,
                        const std::string& callsign)
{
  if (m_sources.find(id) != m_sources.end())
  {
    return;
  }

  Source* src = new Source;
  src->is_local = local;
  src->callsign = callsign;
  // Ring cap: prebuffer plus a generous margin so bursts are absorbed.
  size_t cap = m_prebuf_size + m_frame_size * 8;
  if (cap < m_frame_size * 4)
  {
    cap = m_frame_size * 4;
  }
  src->sink    = new SourceSink(&src->ring, cap);
  src->decoder = AudioDecoder::create("OPUS");
  if (src->decoder != nullptr)
  {
    src->decoder->registerSink(src->sink, false);
  }
  // Encoder is created lazily (only while mixing 2+ talkers); see dropEncoders/
  // ensureSourceEncoder. A source that only ever talks alone never gets one.
  src->encoder = nullptr;
  if (m_mixing) { ensureSourceEncoder(src); } // joined mid-mix -> needs it now
  src->prebuffering    = (m_prebuf_size > 0);
  src->prebuf_target   = m_prebuf_size; // full cushion for the initial start
  src->active_hangover = 0;

  m_sources[id] = src;
  setTickEnabled(true);
} /* TGMixer::addSource */


void TGMixer::removeSource(const std::string& id)
{
  auto it = m_sources.find(id);
  if (it == m_sources.end())
  {
    return;
  }
  // If the source was mid-transmission, announce its stop so talker-stop is
  // sent (e.g. a client disconnecting while keyed up).
  if (it->second->announced_active)
  {
    it->second->announced_active = false;
    sourceActivity(m_tg, id, it->second->callsign, false);
  }
  destroySource(it->second);
  m_sources.erase(it);
  if (m_sources.empty())
  {
    setTickEnabled(false);
  }
} /* TGMixer::removeSource */


bool TGMixer::hasSource(const std::string& id) const
{
  return m_sources.find(id) != m_sources.end();
} /* TGMixer::hasSource */


void TGMixer::writeOpus(const std::string& id,
                        const std::vector<uint8_t>& opus, bool local,
                        const std::string& callsign)
{
  auto it = m_sources.find(id);
  if (it == m_sources.end())
  {
    addSource(id, local, callsign);
    it = m_sources.find(id);
  }
  Source* src = it->second;
  if (!callsign.empty())
  {
    src->callsign = callsign; // keep the latest known callsign
  }
  src->got_packet = true;       // carrier open (COR): a packet arrived this tick
  if (!opus.empty()) { src->last_opus = opus; } // for <=1-talker raw passthrough
  if ((src->decoder != nullptr) && !opus.empty())
  {
    // AudioDecoder::writeEncodedSamples takes a non-const buffer pointer.
    std::vector<uint8_t> tmp(opus);
    src->decoder->writeEncodedSamples(tmp.data(),
                                      static_cast<int>(tmp.size()));
  }
} /* TGMixer::writeOpus */


bool TGMixer::hasActiveSource(const std::string& except) const
{
  for (const auto& kv : m_sources)
  {
    if ((kv.first != except) && kv.second->announced_active)
    {
      return true;
    }
  }
  return false;
} /* TGMixer::hasActiveSource */


/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

Async::AudioEncoder* TGMixer::makeEncoder(void)
{
  AudioEncoder* enc = AudioEncoder::create("OPUS");
  if (enc == nullptr)
  {
    return nullptr;
  }
  // Match the mixer frame clock. (Opus DTX is off by default, which is what
  // we want: the mixer decides activity, not the codec.) Done via the
  // generic setOption API so no codec-private header is needed.
  enc->setOption("FRAME_SIZE", std::to_string(m_frame_ms));
  return enc;
} /* TGMixer::makeEncoder */


void TGMixer::ensureSourceEncoder(Source* src)
{
  if ((src == nullptr) || (src->encoder != nullptr))
  {
    return;
  }
  src->encoder = makeEncoder();
  if (src->encoder != nullptr)
  {
    src->encoder->writeEncodedSamples.connect(
        [src](const void* buf, int size)
        {
          src->cur_out.assign(static_cast<const uint8_t*>(buf),
                              static_cast<const uint8_t*>(buf) + size);
        });
  }
} /* TGMixer::ensureSourceEncoder */


void TGMixer::ensureMixEncoders(void)
{
  if (m_full_encoder == nullptr)
  {
    m_full_encoder = makeEncoder();
    if (m_full_encoder != nullptr)
    {
      m_full_encoder->writeEncodedSamples.connect(
          [this](const void* buf, int size)
          {
            m_cur_full.assign(static_cast<const uint8_t*>(buf),
                              static_cast<const uint8_t*>(buf) + size);
          });
    }
  }
  if (m_local_encoder == nullptr)
  {
    m_local_encoder = makeEncoder();
    if (m_local_encoder != nullptr)
    {
      m_local_encoder->writeEncodedSamples.connect(
          [this](const void* buf, int size)
          {
            m_cur_local.assign(static_cast<const uint8_t*>(buf),
                               static_cast<const uint8_t*>(buf) + size);
          });
    }
  }
} /* TGMixer::ensureMixEncoders */


void TGMixer::dropEncoders(void)
{
  delete m_full_encoder;  m_full_encoder = nullptr;
  delete m_local_encoder; m_local_encoder = nullptr;
  for (auto& kv : m_sources)
  {
    delete kv.second->encoder;
    kv.second->encoder = nullptr;
  }
} /* TGMixer::dropEncoders */


void TGMixer::destroySource(Source* src)
{
  if (src == nullptr)
  {
    return;
  }
  if (src->decoder != nullptr)
  {
    src->decoder->unregisterSink();
  }
  delete src->decoder;
  delete src->sink;
  delete src->encoder;
  delete src;
} /* TGMixer::destroySource */


void TGMixer::setTickEnabled(bool enable)
{
  m_tick_timer.setEnable(enable);
} /* TGMixer::setTickEnabled */


void TGMixer::onTick(Async::Timer*)
{
  if (m_sources.empty())
  {
    return;
  }

  // 1) Pull one frame of PCM from each source, decide which are active.
  std::vector<float> full_sum(m_frame_size, 0.0f);
  std::vector<float> local_sum(m_frame_size, 0.0f);
  bool any_local_audio = false;
  struct ActiveFrame { Source* src; const std::string* id; std::vector<float> pcm; };
  std::vector<ActiveFrame> active;
  std::map<Source*, std::vector<float>> frame_of; // active sources' own pcm
  std::set<Source*> audio_srcs;                   // carrier open THIS frame (COR)

  bool any_audio = false;
  for (auto& kv : m_sources)
  {
    Source* src = kv.second;
    src->audio_now = false; // set true below if its carrier is open this frame

    // Talker state (TalkerStart/Stop) follows the CARRIER = raw packet arrival
    // (COR), not the decoded ring. So TalkerStart fires on the first packet
    // with NO jitter-prebuffer delay (the radio keys immediately, no clipped
    // start), and TalkerStop follows the last packet by a short hang. A local
    // client (radio/app, clean stream) gets a snappy release; mesh/trunk get a
    // long hang to ride network jitter. This matches the passthrough audio
    // timing instead of lagging it by the prebuffer + 1.5 s.
    if (src->got_packet)
    {
      src->talker_hangover = src->is_local ? m_talker_hang_local_frames
                                           : m_talker_hang_frames;
    }
    else if (src->talker_hangover > 0)
    {
      src->talker_hangover--;
    }

    // Respect the prebuffer before a source starts contributing.
    if (src->prebuffering)
    {
      if (src->ring.size() >= src->prebuf_target)
      {
        src->prebuffering = false;
      }
      else
      {
        // Not enough buffered yet: treat as silence for the MIX, decay the mix
        // hangover. (Talker state is handled above from packet arrival.)
        if (src->active_hangover > 0) { src->active_hangover--; }
        // Count toward pruning once it is no longer contributing, even if a
        // stale partial frame lingers in the ring, so a source stuck in
        // prebuffer with the talker long gone is eventually reaped.
        if (src->active_hangover == 0)
        {
          src->idle_frames++;
        }
        continue;
      }
    }

    size_t avail = std::min<size_t>(src->ring.size(), m_frame_size);
    // COR: the carrier is open this frame iff the source actually streamed
    // audio (packets present in the ring). No audio-level/VOX gate.
    bool has_audio = (avail > 0);
    std::vector<float> pcm(m_frame_size, 0.0f);
    for (size_t i = 0; i < avail; ++i)
    {
      pcm[i] = src->ring.front();
      src->ring.pop_front();
    }
    src->audio_now = has_audio;
    if (has_audio) { audio_srcs.insert(src); }
    if (avail < m_frame_size)
    {
      // Short/empty frame. Tolerate transient jitter: the tail of `pcm` is
      // already zero-filled past `avail`, so we still contribute this frame and
      // keep the source active. Only after a SUSTAINED outage do we rebuild the
      // jitter buffer (which would inject a prebuffer of silence). This stops a
      // steady but jittery source — a trunk/bridge or an uneven node, duplex or
      // half-duplex — from being chopped every time a single packet is late.
      src->underrun_frames++;
      if ((m_prebuf_size > 0) && (src->underrun_frames >= m_reprebuf_frames))
      {
        src->prebuffering = true;
        src->prebuf_target = m_reprebuf_target; // resume fast, no 60 ms hole
        src->underrun_frames = 0;
      }
    }
    else
    {
      src->underrun_frames = 0;
    }

    if (has_audio)
    {
      src->active_hangover = m_hangover_frames; // mix contribution (ring-based)
      any_audio = true;
      for (unsigned i = 0; i < m_frame_size; ++i)
      {
        full_sum[i] += pcm[i];
      }
      if (src->is_local)
      {
        any_local_audio = true;
        for (unsigned i = 0; i < m_frame_size; ++i)
        {
          local_sum[i] += pcm[i];
        }
      }
    }
    else
    {
      // Mix contribution fades after the short hangover (so a silence gap adds
      // no latency for other listeners). The TALKER state is handled above from
      // packet arrival (COR), independent of this ring-based mix fade.
      if (src->active_hangover > 0) { src->active_hangover--; }
    }

    // Track idle time for source pruning (a fully idle source can be
    // dropped; receiving is driven by TG membership, not by being a
    // source, so pruning never silences a listener).
    if (has_audio)
    {
      src->idle_frames = 0;
    }
    else if (src->ring.empty() && (src->active_hangover == 0))
    {
      src->idle_frames++;
    }
    else
    {
      src->idle_frames = 0;
    }

    if (src->active_hangover > 0)
    {
      frame_of[src] = pcm; // keep own pcm for mix-minus-self
    }
  }

  // Detect per-source start/stop transitions (debounced by the hangover)
  // and announce them so the reflector can send MsgTalkerStart/Stop exactly
  // like half-duplex. Collected first, emitted after, so slots cannot
  // disturb the iteration.
  {
    std::vector<std::pair<std::string, std::string>> starts, stops;
    unsigned active = 0;
    const std::string* sole_id = nullptr;
    for (auto& kv : m_sources)
    {
      Source* src = kv.second;
      bool now_active = (src->talker_hangover > 0);
      if (now_active) { active++; sole_id = &kv.first; }
      if (now_active && !src->announced_active)
      {
        src->announced_active = true;
        starts.emplace_back(kv.first, src->callsign);
      }
      else if (!now_active && src->announced_active)
      {
        src->announced_active = false;
        stops.emplace_back(kv.first, src->callsign);
      }
    }
    m_active_count = active;
    // Single-talker passthrough applies ONLY when the one talker is a local
    // CLIENT ("c:<id>") -- the Reflector already forwarded its raw Opus to the
    // other clients in handleMsgUdpAudio. A satellite ("s:") source is also
    // is_local but its audio arrives via SatelliteLink, NOT handleMsgUdpAudio,
    // so no raw forward happened: clients MUST be served by onMixReady, so this
    // stays false. (A trunk "u:" source is not local anyway.)
    m_single_local = (active == 1) && (sole_id != nullptr) &&
                     (sole_id->rfind("c:", 0) == 0);
    for (auto& p : stops)  { sourceActivity(m_tg, p.first, p.second, false); }
    for (auto& p : starts) { sourceActivity(m_tg, p.first, p.second, true); }
  }

  // Prune sources that have been idle past the timeout. Safe at any time:
  // a pruned source re-registers on its next packet, and listeners keep
  // receiving because routing is by TG membership, not by source presence.
  {
    std::vector<std::string> dead;
    for (auto& kv : m_sources)
    {
      if (kv.second->idle_frames > m_source_timeout_frames)
      {
        dead.push_back(kv.first);
      }
    }
    for (const auto& id : dead)
    {
      destroySource(m_sources[id]);
      m_sources.erase(id);
    }
    if (m_sources.empty())
    {
      // No contributors left: stop the clock until audio returns. The
      // mixer object is kept and reused (reaped on client disconnect).
      if (m_mixing) { dropEncoders(); m_mixing = false; } // free codecs too
      setTickEnabled(false);
    }
  }

  // Mix only when 2+ talkers actually OVERLAP. With <= 1 talker the Reflector
  // forwards the raw Opus (passthrough), so this mixer needs no encoders at all
  // -- as lean as stock svxref for the common case. Enter mix mode (allocate
  // encoders) on overlap, leave it (free them) when back to a lone talker.
  const bool want_mix = (m_active_count >= 2);
  if (want_mix && !m_mixing)
  {
    ensureMixEncoders();
    for (auto& kv : m_sources) { ensureSourceEncoder(kv.second); }
    m_mixing = true;
  }
  else if (!want_mix && m_mixing)
  {
    dropEncoders();
    m_mixing = false;
  }

  if (!any_audio)
  {
    for (auto& kv : m_sources) { kv.second->got_packet = false; }
    return; // nothing to send this frame
  }

  MixResult result;
  result.tg = m_tg;
  result.active_sources = m_active_count;
  result.passthrough_clients = m_single_local; // sole local talker -> raw forward

  // 2) While mixing: encode the full mix (passive listeners) and the local-only
  //    sub-mix (mesh). Skipped entirely at <= 1 talker (no encoders exist).
  if (m_mixing)
  {
    m_cur_full.clear();
    if (m_full_encoder != nullptr)
    {
      m_full_encoder->writeSamples(full_sum.data(),
                                   static_cast<int>(m_frame_size));
    }
    m_cur_local.clear();
    if (any_local_audio && (m_local_encoder != nullptr))
    {
      m_local_encoder->writeSamples(local_sum.data(),
                                    static_cast<int>(m_frame_size));
    }
    result.full = m_cur_full;
    result.local_mix = m_cur_local;
  }

  // 3) Per active source: on-air duplex learning (COR, runs in BOTH modes so a
  //    take-turns downgrade works even when talkers never overlap) and, while
  //    mixing, the personalised mix-minus-self (no talker cap, ConfBridge).
  std::vector<std::string> fd_hits; // local sources newly proven full duplex
  std::vector<std::string> sg_hits; // local sources newly proven simplex
  for (auto& kv : m_sources)
  {
    Source* src = kv.second;
    if (src->active_hangover == 0)
    {
      continue; // passive listener -> gets full mix (no own audio to remove)
    }

    // COR: is another source's carrier open THIS frame, excluding itself and
    // same-callsign copies (its own audio looped back over a trunk)?
    bool other_now = false;
    for (Source* o : audio_srcs)
    {
      if (o == src) { continue; }
      if (!src->callsign.empty() && (o->callsign == src->callsign)) { continue; }
      other_now = true;
      break;
    }
    const bool self_now = src->audio_now; // this source's carrier open this frame

    // On-air duplex learning (overrides the freq hint in EITHER direction).
    if (src->is_local)
    {
      // UPGRADE -> full duplex: this source's carrier is open THIS frame AND
      // another's is open at the same time -> it transmits and receives at once.
      // Sustained past the observe window makes it stick. A simplex node never
      // reaches this; a brief PTT-edge overlap is filtered out by the sustain.
      if (self_now && other_now && !src->fd_announced)
      {
        if (++src->fd_run >= m_duplex_observe_frames)
        {
          src->fd_announced = true;
          fd_hits.push_back(kv.first);
        }
      }
      else if (!(self_now && other_now))
      {
        src->fd_run = 0; // require SUSTAINED overlap
      }

      // DOWNGRADE -> simplex: over a window this source and others were BOTH
      // active but NEVER in the same frame (classic take-turns / half duplex).
      // Lets a wrong "full" freq hint be corrected. The upgrade above wins and
      // is sticky, so a node that ever truly doubles stays full.
      if (!src->fd_announced && !src->simplex_announced)
      {
        if (self_now)   { src->dg_self++; }
        if (other_now)  { src->dg_other++; }
        if (self_now && other_now) { src->dg_overlap++; }
        if (++src->dg_frames >= m_duplex_window_frames)
        {
          const unsigned minact = m_duplex_observe_frames; // meaningful activity
          if ((src->dg_overlap == 0) &&
              (src->dg_self >= minact) && (src->dg_other >= minact))
          {
            src->simplex_announced = true;
            sg_hits.push_back(kv.first);
          }
          src->dg_frames = src->dg_self = src->dg_other = src->dg_overlap = 0;
        }
      }
    }

    // Mix-minus-self is only produced while mixing. At <= 1 talker there is
    // nothing to mix; the raw passthrough below covers the sole talker.
    if (!m_mixing)
    {
      continue;
    }
    ensureSourceEncoder(src);
    if (src->encoder == nullptr)
    {
      continue;
    }
    const std::vector<float>& own = frame_of[src];
    std::vector<float> minus(m_frame_size, 0.0f);
    for (unsigned i = 0; i < m_frame_size; ++i)
    {
      minus[i] = full_sum[i] - (i < own.size() ? own[i] : 0.0f);
    }
    // Also subtract any OTHER active source carrying the same callsign: this
    // node's own audio looped back over a trunk (separate source id, same
    // callsign). A node must never hear itself, directly or via the mesh.
    if (!src->callsign.empty())
    {
      for (auto& kv2 : m_sources)
      {
        Source* other = kv2.second;
        if ((other == src) || (other->callsign != src->callsign))
        {
          continue;
        }
        auto fit = frame_of.find(other);
        if (fit == frame_of.end())
        {
          continue;
        }
        const std::vector<float>& oth = fit->second;
        for (unsigned i = 0; i < m_frame_size; ++i)
        {
          minus[i] -= (i < oth.size() ? oth[i] : 0.0f);
        }
      }
    }

    // Don't hand a talker encoded silence: no other carrier open -> empty entry
    // (send nothing), never encoded silence or (via a missing entry) the full
    // mix with its own audio.
    src->cur_out.clear();
    if (src->is_local && !other_now)
    {
      result.per_source[kv.first]; // empty -> send nothing
    }
    else
    {
      src->encoder->writeSamples(minus.data(), static_cast<int>(m_frame_size));
      if (!src->cur_out.empty())
      {
        result.per_source[kv.first] = src->cur_out;
      }
    }
  }

  // <= 1 talker: forward the sole talker's latest raw Opus (no decode-mix-
  // encode). Only when a packet actually arrived this tick, so a gap or the
  // hangover tail forwards nothing. The talker itself hears nothing (empty
  // per_source entry). Mesh gets it via local_mix; clients via the Reflector's
  // own passthrough in handleMsgUdpAudio.
  if (!m_mixing)
  {
    for (auto& kv : m_sources)
    {
      Source* src = kv.second;
      if ((src->talker_hangover > 0) && src->got_packet &&
          !src->last_opus.empty())
      {
        result.full = src->last_opus;
        result.local_mix = src->is_local ? src->last_opus
                                          : std::vector<uint8_t>();
        result.per_source[kv.first]; // sole talker hears nothing
        break;
      }
    }
  }

  for (auto& kv : m_sources) { kv.second->got_packet = false; }

  // Announce newly-learned duplex classifications (after the loop so the slot
  // handlers cannot disturb the iteration over m_sources).
  for (const auto& id : fd_hits) { sourceFullDuplex(m_tg, id); }
  for (const auto& id : sg_hits) { sourceSimplex(m_tg, id); }

  mixReady(result);
} /* TGMixer::onTick */


/*
 * This file has not been truncated
 */
