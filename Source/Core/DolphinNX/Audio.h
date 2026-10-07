// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <atomic>

#include <SDL2/SDL.h>

#include "AudioCommon/SoundStream.h"
#include "Common/CommonTypes.h"
#include "Core/Config/MainSettings.h"

namespace DolphinNX
{
namespace Audio
{

class SwitchStream final : public SoundStream
{
public:
  SwitchStream(unsigned int backendSampleRate = 48000);
  ~SwitchStream() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  static bool IsValid() { return true; }

private:
  static void AudioCallback(void* userdata, u8* stream, int len);

  SDL_AudioDeviceID m_device = 0;
  std::atomic<bool> m_running{false};
};

// Plays a sound over the game's (e.g. tico's trophy sound): a 16-bit PCM WAV,
// resampled to the output's 48 kHz stereo. Replaces one still playing.
bool PlayEffect(const std::string& wav_path);

}  // namespace Audio
}  // namespace DolphinNX
