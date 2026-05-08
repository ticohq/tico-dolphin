// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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

}  // namespace Audio
}  // namespace DolphinNX
