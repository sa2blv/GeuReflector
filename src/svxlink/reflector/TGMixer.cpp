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
  // Hangover: keep a source "active" ~200 ms past its last audio so brief
  // gaps between packets do not flap the mix-minus-self routing.
  m_hangover_frames = (200 + m_frame_ms - 1) / m_frame_ms;
  m_source_timeout_frames = (src_timeout_ms + m_frame_ms - 1) / m_frame_ms;

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
  src->prebuffering    = (m_prebuf_size > 0);
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
  if ((src->decoder != nullptr) && !opus.empty())
  {
    // AudioDecoder::writeEncodedSamples takes a non-const buffer pointer.
    std::vector<uint8_t> tmp(opus);
    src->decoder->writeEncodedSamples(tmp.data(),
                                      static_cast<int>(tmp.size()));
  }
} /* TGMixer::writeOpus */


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

  bool any_audio = false;
  for (auto& kv : m_sources)
  {
    Source* src = kv.second;

    // Respect the prebuffer before a source starts contributing.
    if (src->prebuffering)
    {
      if (src->ring.size() >= m_prebuf_size)
      {
        src->prebuffering = false;
      }
      else
      {
        // Not enough buffered yet: treat as silence, decay hangover.
        if (src->active_hangover > 0) { src->active_hangover--; }
        if (src->ring.empty() && (src->active_hangover == 0))
        {
          src->idle_frames++;
        }
        continue;
      }
    }

    size_t avail = std::min<size_t>(src->ring.size(), m_frame_size);
    bool has_audio = (avail > 0);
    std::vector<float> pcm(m_frame_size, 0.0f);
    for (size_t i = 0; i < avail; ++i)
    {
      pcm[i] = src->ring.front();
      src->ring.pop_front();
    }
    if (avail < m_frame_size)
    {
      // Underrun: rebuild the jitter buffer before contributing again.
      if (m_prebuf_size > 0) { src->prebuffering = true; }
    }

    if (has_audio)
    {
      src->active_hangover = m_hangover_frames;
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
    else if (src->active_hangover > 0)
    {
      src->active_hangover--;
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
    for (auto& kv : m_sources)
    {
      Source* src = kv.second;
      bool now_active = (src->active_hangover > 0);
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
      setTickEnabled(false);
    }
  }

  if (!any_audio)
  {
    return; // nothing to send this frame
  }

  // 2) Encode the full mix for passive listeners.
  m_cur_full.clear();
  if (m_full_encoder != nullptr)
  {
    m_full_encoder->writeSamples(full_sum.data(),
                                 static_cast<int>(m_frame_size));
  }

  // Encode the local-only sub-mix for the mesh (trunk/satellite/twin).
  m_cur_local.clear();
  if (any_local_audio && (m_local_encoder != nullptr))
  {
    m_local_encoder->writeSamples(local_sum.data(),
                                  static_cast<int>(m_frame_size));
  }

  MixResult result;
  result.tg = m_tg;
  result.full = m_cur_full;
  result.local_mix = m_cur_local;

  // 3) Per active source: encode full mix minus its own audio. Cap the
  //    number of personalised encodes; overflow sources get the full mix.
  unsigned produced = 0;
  for (auto& kv : m_sources)
  {
    Source* src = kv.second;
    if (src->active_hangover == 0)
    {
      continue; // passive listener -> gets full mix
    }
    if (produced >= m_max_talkers)
    {
      break;
    }
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
    src->cur_out.clear();
    src->encoder->writeSamples(minus.data(), static_cast<int>(m_frame_size));
    if (!src->cur_out.empty())
    {
      result.per_source[kv.first] = src->cur_out;
    }
    produced++;
  }

  mixReady(result);
} /* TGMixer::onTick */


/*
 * This file has not been truncated
 */
