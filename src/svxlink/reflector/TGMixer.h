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
  // Number of sources currently active (talker state held) on this TG.
  unsigned                                            active_sources = 0;
  // true when exactly one LOCAL client is talking: its raw Opus is forwarded
  // straight to the other clients by the Reflector (zero added latency), so
  // onMixReady must NOT also distribute the (re-encoded) mix to clients. Mesh
  // links still get the mix. A sole MESH talker keeps this false (clients can
  // only reach it via the mix path).
  bool                                                passthrough_clients = false;
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
     * @brief   Number of sources currently active (talking), refreshed each
     *          onTick. <= 1 means the Reflector may forward a single client's
     *          raw Opus directly instead of the mixed stream (low latency).
     */
    unsigned activeCount(void) const { return m_active_count; }

    /**
     * @brief   Is any source currently producing audio?
     * @param   except A source id to ignore (e.g. the caller's own source)
     *
     * Uses the same debounced activity as sourceActivity, so it is true
     * from talker-start to talker-stop of any source on this TG.
     */
    bool hasActiveSource(const std::string& except = "") const;

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

    /**
     * @brief   Emitted when a LOCAL source is first observed to be full duplex.
     * @param   tg This mixer's talk group
     * @param   id The source id ("c:<clientId>")
     *
     * Fires once, the first time a local source delivers its own audio while
     * there is OTHER audible audio for it to receive on the TG (i.e. it is
     * transmitting AND receiving at once), sustained past a short debounce.
     * The reflector uses it to mark the client full duplex so it keeps getting
     * mix-minus-self while it transmits. Pure on-air learning — no node info.
     */
    sigc::signal<void(uint32_t, const std::string&)> sourceFullDuplex;

    /**
     * @brief   Emitted when a LOCAL source is observed to be half duplex.
     * @param   tg This mixer's talk group
     * @param   id The source id ("c:<clientId>")
     *
     * Fires once when, over a window, this source and other sources were BOTH
     * active but never in the same frame (classic take-turns / half duplex).
     * Lets the reflector override a wrong "full" freq hint back to simplex.
     */
    sigc::signal<void(uint32_t, const std::string&)> sourceSimplex;

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
      size_t                prebuf_target = 0; // samples to buffer before (re)starting
      bool                  is_local = true;   // local client vs mesh source
      std::string           callsign;          // for talker-start/stop
      bool                  announced_active = false; // start/stop debounce
      unsigned              active_hangover = 0; // frames of mix contribution left
      unsigned              talker_hangover = 0; // frames of TALKER state left (longer)
      unsigned              idle_frames = 0;   // consecutive fully-idle frames
      unsigned              underrun_frames = 0; // consecutive short/empty frames
      bool                  audio_now = false; // carrier open THIS frame (COR: it streamed)
      unsigned              fd_run = 0;        // consecutive frames active-while-others-active
      bool                  fd_announced = false; // full-duplex already signalled
      // Half-duplex (take-turns) detection window: both this source and others
      // were active within the window but never in the SAME frame => simplex.
      unsigned              dg_frames = 0;     // frames counted in this window
      unsigned              dg_self = 0;       // frames this source had carrier
      unsigned              dg_other = 0;      // frames others had carrier
      unsigned              dg_overlap = 0;    // frames both at once
      bool                  simplex_announced = false; // simplex already signalled
      bool                  got_packet = false; // a packet arrived since last tick
      std::vector<uint8_t>  last_opus;         // most recent raw Opus (for passthrough)
      std::vector<uint8_t>  cur_out;           // encoder output this frame
    };

    uint32_t                          m_tg;
    unsigned                          m_frame_ms;
    unsigned                          m_jitter_ms;
    unsigned                          m_max_talkers;
    unsigned                          m_frame_size;    // samples per frame
    unsigned                          m_prebuf_size;   // samples before INITIAL play
    unsigned                          m_reprebuf_target; // samples before RESUME after a gap
    unsigned                          m_hangover_frames;
    unsigned                          m_talker_hang_frames;       // mesh/trunk TalkerStop debounce
    unsigned                          m_talker_hang_local_frames; // local client (radio/app) debounce
    unsigned                          m_active_count = 0;   // sources talking now
    bool                              m_single_local = false; // the sole talker is a local client
    unsigned                          m_reprebuf_frames; // sustained underrun -> rebuild jitter
    unsigned                          m_source_timeout_frames; // idle->prune
    unsigned                          m_duplex_observe_frames; // heard-while-talking frames -> full duplex
    unsigned                          m_duplex_window_frames;  // take-turns detection window length
    std::map<std::string, Source*>    m_sources;
    Async::AudioEncoder*              m_full_encoder = nullptr;
    Async::AudioEncoder*              m_local_encoder = nullptr;
    bool                              m_mixing = false; // 2+ talkers: encoders live
    std::vector<uint8_t>              m_cur_full;      // full-mix enc output
    std::vector<uint8_t>              m_cur_local;     // local-mix enc output
    Async::Timer                      m_tick_timer;

    TGMixer(const TGMixer&);
    TGMixer& operator=(const TGMixer&);

    Async::AudioEncoder* makeEncoder(void);
    void ensureSourceEncoder(Source* src); // lazily create a source's mix encoder
    void ensureMixEncoders(void);          // lazily create the full/local encoders
    void dropEncoders(void);               // free all encoders when mixing stops
    void destroySource(Source* src);
    void onTick(Async::Timer*);
    void setTickEnabled(bool enable);

}; /* class TGMixer */


#endif /* TG_MIXER_INCLUDED */

/*
 * This file has not been truncated
 */
