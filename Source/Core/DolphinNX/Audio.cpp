// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Audio.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <switch.h>

#include "Common/FileUtil.h"

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

namespace DolphinNX
{
namespace Audio
{
namespace
{
constexpr u32 kOutputRate = 48000;
// as porpoise's audout thread, above the default 0x2C
constexpr s32 kAudioThreadPriority = 0x26;

std::mutex s_effect_mutex;
std::vector<s16> s_effect;  // interleaved stereo at kOutputRate
std::size_t s_effect_pos = 0;

// Reads a 16-bit PCM WAV and resamples it (linearly) to kOutputRate stereo.
bool LoadWav(const std::string& path, std::vector<s16>& out)
{
  std::string data;
  if (!File::ReadFileToString(path, data) || data.size() < 12 || data.compare(0, 4, "RIFF") != 0 ||
      data.compare(8, 4, "WAVE") != 0)
  {
    return false;
  }
  u16 channels = 0;
  u16 bits = 0;
  u32 rate = 0;
  const char* samples = nullptr;
  std::size_t sample_bytes = 0;
  for (std::size_t pos = 12; pos + 8 <= data.size();)
  {
    u32 size = 0;
    std::memcpy(&size, data.data() + pos + 4, 4);
    const std::size_t body = pos + 8;
    if (data.compare(pos, 4, "fmt ") == 0 && size >= 16 && body + 16 <= data.size())
    {
      std::memcpy(&channels, data.data() + body + 2, 2);
      std::memcpy(&rate, data.data() + body + 4, 4);
      std::memcpy(&bits, data.data() + body + 14, 2);
    }
    else if (data.compare(pos, 4, "data") == 0)
    {
      samples = data.data() + body;
      sample_bytes = std::min<std::size_t>(size, data.size() - body);
    }
    pos = body + size + (size & 1);
  }
  if (!samples || bits != 16 || (channels != 1 && channels != 2) || rate == 0)
    return false;

  const std::size_t frames = sample_bytes / (2 * channels);
  auto sample = [&](std::size_t frame, int channel) {
    s16 value;
    std::memcpy(&value, samples + (frame * channels + (channels == 2 ? channel : 0)) * 2, 2);
    return value;
  };
  const std::size_t out_frames = frames * kOutputRate / rate;
  out.resize(out_frames * 2);
  for (std::size_t i = 0; i < out_frames; ++i)
  {
    const double source = static_cast<double>(i) * rate / kOutputRate;
    const std::size_t a = std::min(static_cast<std::size_t>(source), frames - 1);
    const std::size_t b = std::min(a + 1, frames - 1);
    const double t = source - static_cast<double>(a);
    for (int channel = 0; channel < 2; ++channel)
    {
      out[i * 2 + channel] =
          static_cast<s16>(sample(a, channel) * (1.0 - t) + sample(b, channel) * t);
    }
  }
  return !out.empty();
}

// Adds the playing effect over the mixed game audio.
void MixEffect(s16* stream, std::size_t sample_count)
{
  std::lock_guard lock(s_effect_mutex);
  if (s_effect_pos >= s_effect.size())
    return;
  const std::size_t count = std::min(sample_count, s_effect.size() - s_effect_pos);
  for (std::size_t i = 0; i < count; ++i)
  {
    const int mixed = stream[i] + s_effect[s_effect_pos + i];
    stream[i] = static_cast<s16>(std::clamp(mixed, -32768, 32767));
  }
  s_effect_pos += count;
}
}  // namespace

bool PlayEffect(const std::string& wav_path)
{
  std::vector<s16> effect;
  if (!LoadWav(wav_path, effect))
    return false;
  std::lock_guard lock(s_effect_mutex);
  s_effect = std::move(effect);
  s_effect_pos = 0;
  return true;
}


SwitchStream::SwitchStream(unsigned int backendSampleRate)
    : SoundStream(backendSampleRate)
{
}

SwitchStream::~SwitchStream()
{
  if (m_device)
  {
    m_running.store(false, std::memory_order_release);
    SDL_PauseAudioDevice(m_device, 1);
    SDL_LockAudioDevice(m_device);
    SDL_UnlockAudioDevice(m_device);
    SDL_CloseAudioDevice(m_device);
    m_device = 0;
  }

  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool SwitchStream::Init()
{
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
  {
    ERROR_LOG_FMT(AUDIO, "SDL audio init failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioSpec want{};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = AudioCallback;
  want.userdata = this;

  SDL_AudioSpec have{};
  m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (m_device == 0)
  {
    ERROR_LOG_FMT(AUDIO, "SDL_OpenAudioDevice failed: {}", SDL_GetError());
    return false;
  }

  INFO_LOG_FMT(AUDIO, "SwitchNX audio: {}Hz, {} channels, {} samples",
               have.freq, have.channels, have.samples);

  return true;
}

bool SwitchStream::SetRunning(bool running)
{
  if (running)
  {
    m_running.store(true, std::memory_order_release);
    if (m_device)
      SDL_PauseAudioDevice(m_device, 0);
  }
  else
  {
    if (m_device)
      SDL_PauseAudioDevice(m_device, 1);
    m_running.store(false, std::memory_order_release);
  }
  return true;
}

void SwitchStream::AudioCallback(void* userdata, u8* stream, int len)
{
  static std::atomic<bool> s_thread_pinned{false};
  if (!s_thread_pinned.exchange(true, std::memory_order_acq_rel))
  {
    Common::SetCurrentThreadName("Audio thread - switchnx");
    // with the host and worker threads, away from the CPU (0) and GPU (1) threads
    Common::SetCurrentThreadAffinity(2);
    // above them: Horizon doesn't share a core between threads of the same
    // priority, so a long burst there (deko3d's shader compiles) starved the
    // output until it went silent
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, kAudioThreadPriority)))
      WARN_LOG_FMT(AUDIO, "Could not raise the audio thread's priority");
  }

  auto* self = static_cast<SwitchStream*>(userdata);
  if (!self->m_running.load(std::memory_order_acquire) || !self->GetMixer())
  {
    std::memset(stream, 0, len);
    return;
  }

  const unsigned int num_samples = len / 4;
  self->GetMixer()->Mix(reinterpret_cast<s16*>(stream), num_samples);
  MixEffect(reinterpret_cast<s16*>(stream), static_cast<std::size_t>(num_samples) * 2);
}

}  // namespace Audio
}  // namespace DolphinNX
