/**
@file   AnnouncePlayer.h
@brief  Plays one audio clip (WAV) as a paced stream of encoded frames
@date   2026-11-08

An AnnouncePlayer loads a WAV file, converts it to mono float PCM at
INTERNAL_SAMPLE_RATE, and on a fixed frame clock encodes one frame at a time
with the reflector's codec. Each encoded packet is emitted through
frameReady() so the owner can route it to clients and/or links exactly like
audio from a real talker. The player knows nothing about the reflector.

\verbatim
This file is part of GeuReflector / SvxLink.
Licensed under the GNU General Public License v2 or later.
\endverbatim
*/

#ifndef ANNOUNCE_PLAYER_INCLUDED
#define ANNOUNCE_PLAYER_INCLUDED

#include <sigc++/sigc++.h>

#include <cstdint>
#include <string>
#include <vector>

#include <AsyncTimer.h>

namespace Async
{
  class AudioEncoder;
};


class AnnouncePlayer : public sigc::trackable
{
  public:
    /**
     * @brief   Load a WAV file into mono float PCM at INTERNAL_SAMPLE_RATE
     * @param   path     Path to a 16-bit PCM WAV file (any rate, mono/stereo)
     * @param   gain_db  Gain applied while loading (0 = unchanged)
     * @param   max_sec  Longest clip accepted, in seconds
     * @param   pcm      Output samples
     * @param   err      Error text on failure
     * @return  true on success
     */
    static bool loadWav(const std::string& path, float gain_db,
                        unsigned max_sec, std::vector<float>& pcm,
                        std::string& err);

    /**
     * @brief   Constructor
     * @param   codec     Codec name (e.g. "OPUS"), as for AudioEncoder::create
     * @param   frame_ms  Frame length / tick interval in milliseconds
     * @param   pcm       The samples to play (see loadWav)
     * @param   tail_ms   Silence appended after the clip
     */
    AnnouncePlayer(const std::string& codec, unsigned frame_ms,
                   std::vector<float> pcm, unsigned tail_ms = 240);
    ~AnnouncePlayer(void);

    /** True if the encoder could be created */
    bool isValid(void) const { return m_enc != nullptr; }

    /** Start emitting frames (no-op if already playing) */
    void start(void);

    /** Stop immediately. finished() is NOT emitted. */
    void stop(void);

    bool isPlaying(void) const { return m_playing; }

    /** Length of the clip incl. tail, in milliseconds */
    unsigned lengthMs(void) const;

    /** One encoded packet, ready to be put in a MsgUdpAudio */
    sigc::signal<void(const std::vector<uint8_t>&)> frameReady;

    /** All frames have been emitted */
    sigc::signal<void()> finished;

  private:
    Async::AudioEncoder*  m_enc = nullptr;
    std::vector<float>    m_pcm;
    size_t                m_pos = 0;
    unsigned              m_frame_size;
    Async::Timer          m_timer;
    bool                  m_playing = false;

    AnnouncePlayer(const AnnouncePlayer&);
    AnnouncePlayer& operator=(const AnnouncePlayer&);

    void onTick(Async::Timer*);
    void onEncoded(const void* buf, int size);
};

#endif /* ANNOUNCE_PLAYER_INCLUDED */
