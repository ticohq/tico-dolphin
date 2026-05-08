// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Audio.h"

#include <atomic>
#include <cstring>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

namespace DolphinNX
{
namespace Audio
{

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
    Common::SetCurrentThreadAffinity(2);
  }

  auto* self = static_cast<SwitchStream*>(userdata);
  if (!self->m_running.load(std::memory_order_acquire) || !self->GetMixer())
  {
    std::memset(stream, 0, len);
    return;
  }

  const unsigned int num_samples = len / 4;
  self->GetMixer()->Mix(reinterpret_cast<s16*>(stream), num_samples);
}

}  // namespace Audio
}  // namespace DolphinNX
