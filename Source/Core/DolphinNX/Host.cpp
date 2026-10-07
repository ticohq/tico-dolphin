// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <switch.h>

#include "Common/Logging/Log.h"
#include "Core/Host.h"
#include "DolphinNX/Overlay/VulkanOverlay.h"

// External running flag from main.cpp
extern bool s_running;

std::vector<std::string> Host_GetPreferredLocales()
{
  // the console's language, e.g. "en-US" or "zh-Hans"
  if (R_FAILED(setInitialize()))
    return {};
  u64 language_code = 0;
  const Result rc = setGetSystemLanguage(&language_code);
  setExit();
  if (R_FAILED(rc))
    return {};

  char tag[sizeof(language_code) + 1] = {};
  std::memcpy(tag, &language_code, sizeof(language_code));
  return {tag};
}

void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}

bool Host_TASInputHasFocus()
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_running = false;
}

void Host_UpdateTitle(const std::string&) {}

void Host_UpdateDiscordClientID(const std::string&) {}

bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&,
                                   const std::string&, const std::string&,
                                   const std::string&, const std::string&,
                                   const int64_t, const int64_t,
                                   const int, const int)
{
  return false;
}

void Host_UpdateDisasmDialog() {}
void Host_RequestRenderWindowSize(int, int) {}

bool Host_RendererHasFocus()
{
  return true;
}

bool Host_RendererHasFullFocus()
{
  return true;
}

bool Host_RendererIsFullscreen()
{
  return true;
}

void Host_YieldToUI() {}
void Host_TitleChanged() {}

bool Host_UIBlocksControllerState()
{
  return DolphinNX::VulkanOverlay::IsVisible();
}
