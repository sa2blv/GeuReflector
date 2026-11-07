/**
@file   TGMixer.h
@brief  Full-duplex per-talkgroup audio conference mixer for SvxReflector
@author Tobias Blomberg / SM0SVX (SvxLink), full-duplex extension
@date   2026-10-31

This class implements a source-agnostic audio conference for a single talk
group. Every audio stream belonging to the TG — a local client, a trunk
peer, a satellite or the twin — is registered as a named source. On a fixed
frame clock all active sources are summed (mixed) and re-encoded so that:

  * passive listeners (sources with no current audio) receive the full mix,
  * each active source receives the full mix minus its own audio
    (mix-minus-self, so a duplex/app node never hears its own echo).

The mixer owns one Opus decoder + jitter ring per source and one Opus
encoder per active source plus one for the full mix. It emits a single
atomic MixResult per frame; routing to the actual recipients is done by the
Reflector, which knows the clients/links. The mixer decodes to PCM, so it is
also the natural place for CTCSS/DTMF filtering (added separately).

\verbatim
This file is part of GeuReflector / SvxLink.
Licensed under the GNU General Public License v2 or later.
\endverbatim
*/

#ifndef TG_MIXER_INCLUDED
#define TG_MIXER_INCLUDED


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <sigc++/sigc++.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncTimer.h>
#include <AsyncAudioSink.h>


/****************************************************************************
 *
 * Forward declarations
 *
 ****************************************************************************/

namespace Async
{
  class Config;
  class AudioEncoder;
  class AudioDecoder;
};


/****************************************************************************
 *
 * Class definitions
 *
 ****************************************************************************/

/**
 * @brief   The mixed output produced for one frame of one talk group.
 *
 * `full` is the Opus-encoded full mix for passive listeners. `per_source`
 * maps an active source id to its personalised Opus-encoded mix-minus-self
 * stream. A recipient that appears as a key in `per_source` must be sent
 * that stream; every other recipient on the TG gets `full`.
 */
struct MixResult
{
  uint32_t                                            tg = 0;
  std::vector<uint8_t>                                full;
  std::map<std::string, std::vector<uint8_t>>         per_source;
  // Sum of this reflector's LOCAL sources only (no mesh-sourced audio).
  // This is what is sent out onto trunk/satellite/twin links: because it
  // never contains audio received from a link, it can never loop back
  // through the mesh, and a remote peer never hears its own audio.
  std::vector<uint8_t>                                local_mix;
};


class TGMixer : public sigc::trackable
{
  public:
    /**
     * @brief   Constructor
     * @param   tg  The talk group this mixer serves
     * @param   cfg The reflector configuration (for MIX_* tunables)
     *
     * Tunables are read per-TG from section "TG#<tg>" with fallback to
     * "GLOBAL": MIX_FRAME_MS (default 20), MIX_JITTER_MS (default 60),
     * MIX_MAX_TALKERS (default 4). All are conf driven; code only clamps
     * to safe ranges.
     */
    TGMixer(uint32_t tg, const Async::Config& cfg);

    /**
     * @brief   Destructor
     */
    ~TGMixer(void);

    /**
     * @brief   Register a new audio source on this TG (idempotent)
     * @param   id    A stable, source-agnostic identity (client or peer id)
     * @param   local true for a local client, false for a mesh-sourced
     *                stream (trunk/satellite/twin). Only local sources are
     *                summed into MixResult::local_mix.
     */
    void addSource(const std::string& id, bool local = true,
                   const std::string& callsign = "");

    /**
     * @brief   Remove a source (e.g. client/link disconnected)
     * @param   id The source identity previously passed to addSource()
     */
    void removeSource(const std::string& id);

    /**
     * @brief   Is the given source currently registered?
     */
    bool hasSource(const std::string& id) const;

    /**
     * @brief   Number of currently registered sources
     */
    size_t sourceCount(void) const { return m_sources.size(); }

    /**
     * @brief   Feed one Opus packet from a source into the mix
     * @param   id    The source identity (auto-registered if unknown)
     * @param   opus  The raw Opus payload as received on the wire
     * @param   local true for a local client, false for a mesh source
     *                (used only if the source must be auto-registered)
     */
    void writeOpus(const std::string& id, const std::vector<uint8_t>& opus,
                   bool local = true, const std::string& callsign = "");

    /**
     * @brief   The talk group id this mixer serves
     */
    uint32_t tg(void) const { return m_tg; }

    /**
     * @brief   Emitted once per frame with the mixed/encoded output
     *
     * Not emitted for a frame in which no source produced any audio.
     */
    sigc::signal<void(const MixResult&)> mixReady;

    /**
     * @brief   Emitted when a source starts or stops producing audio.
     * @param   tg       This mixer's talk group
     * @param   id       The source id (e.g. "c:<clientId>")
     * @param   callsign The source's callsign (empty if unknown)
     * @param   active   true on idle->active, false on active->idle
     *
     * Debounced by the activity hangover so it does not flap between
     * packets. Used to drive MsgTalkerStart/Stop exactly like half-duplex.
     */
    sigc::signal<void(uint32_t, const std::string&, const std::string&, bool)>
        sourceActivity;

  private:
    static const unsigned SAMPLE_RATE = INTERNAL_SAMPLE_RATE;

    class SourceSink; // AudioSink capturing decoded PCM into a ring

    struct Source
    {
      Async::AudioDecoder*  decoder = nullptr;
      SourceSink*           sink    = nullptr;
      Async::AudioEncoder*  encoder = nullptr; // for mix-minus-self
      std::deque<float>     ring;              // jitter buffer (PCM)
      bool                  prebuffering = true;
      bool                  is_local = true;   // local client vs mesh source
      std::string           callsign;          // for talker-start/stop
      bool                  announced_active = false; // start/stop debounce
      unsigned              active_hangover = 0; // frames since last audio
      unsigned              idle_frames = 0;   // consecutive fully-idle frames
      unsigned              underrun_frames = 0; // consecutive short/empty frames
      std::vector<uint8_t>  cur_out;           // encoder output this frame
    };

    uint32_t                          m_tg;
    unsigned                          m_frame_ms;
    unsigned                          m_jitter_ms;
    unsigned                          m_max_talkers;
    unsigned                          m_frame_size;    // samples per frame
    unsigned                          m_prebuf_size;   // samples before play
    unsigned                          m_hangover_frames;
    unsigned                          m_reprebuf_frames; // sustained underrun -> rebuild jitter
    unsigned                          m_source_timeout_frames; // idle->prune
    std::map<std::string, Source*>    m_sources;
    Async::AudioEncoder*              m_full_encoder = nullptr;
    Async::AudioEncoder*              m_local_encoder = nullptr;
    std::vector<uint8_t>              m_cur_full;      // full-mix enc output
    std::vector<uint8_t>              m_cur_local;     // local-mix enc output
    Async::Timer                      m_tick_timer;

    TGMixer(const TGMixer&);
    TGMixer& operator=(const TGMixer&);

    Async::AudioEncoder* makeEncoder(void);
    void destroySource(Source* src);
    void onTick(Async::Timer*);
    void setTickEnabled(bool enable);

}; /* class TGMixer */


#endif /* TG_MIXER_INCLUDED */

/*
 * This file has not been truncated
 */
