/**
@file   AnnouncePlayer.cpp
@brief  Plays one audio clip (WAV) as a paced stream of encoded frames
@date   2026-11-08

\verbatim
This file is part of GeuReflector / SvxLink.
Licensed under the GNU General Public License v2 or later.
\endverbatim
*/

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <AsyncAudioEncoder.h>

#include "AnnouncePlayer.h"


namespace {
  uint32_t rdU32(const unsigned char* p)
  {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  uint16_t rdU16(const unsigned char* p)
  {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
  }
}


bool AnnouncePlayer::loadWav(const std::string& path, float gain_db,
                             unsigned max_sec, std::vector<float>& pcm,
                             std::string& err)
{
  pcm.clear();
  std::ifstream f(path, std::ios::binary);
  if (!f)
  {
    err = "cannot open '" + path + "'";
    return false;
  }

  unsigned char hdr[12];
  if (!f.read(reinterpret_cast<char*>(hdr), 12) ||
      (std::memcmp(hdr, "RIFF", 4) != 0) ||
      (std::memcmp(hdr + 8, "WAVE", 4) != 0))
  {
    err = "'" + path + "' is not a RIFF/WAVE file";
    return false;
  }

  uint16_t fmt_tag = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  bool have_fmt = false;
  std::vector<unsigned char> data;

  unsigned char ch[8];
  while (f.read(reinterpret_cast<char*>(ch), 8))
  {
    uint32_t len = rdU32(ch + 4);
    if (std::memcmp(ch, "fmt ", 4) == 0)
    {
      std::vector<unsigned char> fmt(len);
      if ((len < 16) || !f.read(reinterpret_cast<char*>(fmt.data()), len))
      {
        err = "'" + path + "': bad fmt chunk";
        return false;
      }
      fmt_tag  = rdU16(&fmt[0]);
      channels = rdU16(&fmt[2]);
      rate     = rdU32(&fmt[4]);
      bits     = rdU16(&fmt[14]);
      if ((fmt_tag == 0xFFFE) && (len >= 26))
      {
        fmt_tag = rdU16(&fmt[24]);  // WAVE_FORMAT_EXTENSIBLE sub format
      }
      have_fmt = true;
    }
    else if (std::memcmp(ch, "data", 4) == 0)
    {
      data.resize(len);
      f.read(reinterpret_cast<char*>(data.data()), len);
      data.resize(static_cast<size_t>(f.gcount()));
      break;
    }
    else
    {
      f.seekg(len, std::ios::cur);
    }
    if (len & 1)
    {
      f.seekg(1, std::ios::cur);   // chunks are word aligned
    }
  }

  if (!have_fmt || data.empty())
  {
    err = "'" + path + "': missing fmt or data chunk";
    return false;
  }
  if ((fmt_tag != 1) || (bits != 16) || (channels < 1) || (channels > 2) ||
      (rate < 8000) || (rate > 96000))
  {
    err = "'" + path + "': only 16-bit PCM WAV, mono/stereo, 8-96 kHz is "
          "supported (got fmt=" + std::to_string(fmt_tag) + " bits=" +
          std::to_string(bits) + " ch=" + std::to_string(channels) +
          " rate=" + std::to_string(rate) + ")";
    return false;
  }

  // Decode to mono float
  const size_t frames = data.size() / (2u * channels);
  if (frames > size_t(max_sec) * rate)
  {
    err = "'" + path + "' is longer than " + std::to_string(max_sec) +
          " s (MAX_LENGTH)";
    return false;
  }
  std::vector<float> mono(frames);
  for (size_t i = 0; i < frames; ++i)
  {
    float sum = 0.0f;
    for (unsigned c = 0; c < channels; ++c)
    {
      int16_t s = static_cast<int16_t>(rdU16(&data[(i * channels + c) * 2]));
      sum += s / 32768.0f;
    }
    mono[i] = sum / channels;
  }

  // Resample (linear) to the internal rate
  const double out_rate = INTERNAL_SAMPLE_RATE;
  if (rate == INTERNAL_SAMPLE_RATE)
  {
    pcm.swap(mono);
  }
  else
  {
    const double step = double(rate) / out_rate;
    const size_t n_out = static_cast<size_t>(frames / step);
    pcm.resize(n_out);
    for (size_t i = 0; i < n_out; ++i)
    {
      double x = i * step;
      size_t i0 = static_cast<size_t>(x);
      size_t i1 = std::min(i0 + 1, frames - 1);
      float frac = static_cast<float>(x - i0);
      pcm[i] = mono[i0] * (1.0f - frac) + mono[i1] * frac;
    }
  }

  if (gain_db != 0.0f)
  {
    const float g = std::pow(10.0f, gain_db / 20.0f);
    for (auto& s : pcm)
    {
      s = std::max(-1.0f, std::min(1.0f, s * g));
    }
  }
  return true;
} /* AnnouncePlayer::loadWav */


AnnouncePlayer::AnnouncePlayer(const std::string& codec, unsigned frame_ms,
                               std::vector<float> pcm, unsigned tail_ms)
  : m_pcm(std::move(pcm)),
    m_frame_size(INTERNAL_SAMPLE_RATE * frame_ms / 1000),
    m_timer(static_cast<int>(frame_ms), Async::Timer::TYPE_PERIODIC, false)
{
  m_enc = Async::AudioEncoder::create(codec);
  if (m_enc != nullptr)
  {
    m_enc->setOption("FRAME_SIZE", std::to_string(frame_ms));
    m_enc->writeEncodedSamples.connect(
        sigc::mem_fun(*this, &AnnouncePlayer::onEncoded));
  }
  // Silence tail, then pad to a whole number of frames so the encoder
  // never keeps a partial frame buffered.
  m_pcm.resize(m_pcm.size() + INTERNAL_SAMPLE_RATE * tail_ms / 1000, 0.0f);
  if (m_frame_size > 0)
  {
    size_t rem = m_pcm.size() % m_frame_size;
    if (rem != 0)
    {
      m_pcm.resize(m_pcm.size() + (m_frame_size - rem), 0.0f);
    }
  }
  m_timer.expired.connect(sigc::mem_fun(*this, &AnnouncePlayer::onTick));
} /* AnnouncePlayer::AnnouncePlayer */


AnnouncePlayer::~AnnouncePlayer(void)
{
  m_timer.setEnable(false);
  delete m_enc;
} /* AnnouncePlayer::~AnnouncePlayer */


void AnnouncePlayer::start(void)
{
  if (m_playing || (m_enc == nullptr))
  {
    return;
  }
  m_playing = true;
  m_pos = 0;
  m_timer.setEnable(true);
} /* AnnouncePlayer::start */


void AnnouncePlayer::stop(void)
{
  m_timer.setEnable(false);
  m_playing = false;
} /* AnnouncePlayer::stop */


unsigned AnnouncePlayer::lengthMs(void) const
{
  return static_cast<unsigned>(m_pcm.size() * 1000 / INTERNAL_SAMPLE_RATE);
} /* AnnouncePlayer::lengthMs */


void AnnouncePlayer::onTick(Async::Timer*)
{
  if (!m_playing)
  {
    return;
  }
  if (m_pos >= m_pcm.size())
  {
    // One extra tick after the last frame so it is paced like the others
    m_timer.setEnable(false);
    m_playing = false;
    finished();   // owner may delete us from here: touch nothing after
    return;
  }
  size_t n = std::min<size_t>(m_frame_size, m_pcm.size() - m_pos);
  m_enc->writeSamples(&m_pcm[m_pos], static_cast<int>(n));
  m_pos += n;
} /* AnnouncePlayer::onTick */


void AnnouncePlayer::onEncoded(const void* buf, int size)
{
  if (size <= 0)
  {
    return;
  }
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  frameReady(std::vector<uint8_t>(b, b + size));
} /* AnnouncePlayer::onEncoded */
