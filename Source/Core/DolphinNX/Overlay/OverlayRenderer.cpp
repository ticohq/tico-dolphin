// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

// The renderer tico's overlay draws with: SDL's while the game list runs before a
// game (LibraryScreen), Dolphin's Vulkan presentation during a game (VulkanOverlay).

#include "overlay/overlay_renderer.h"

#include "DolphinNX/LibraryScreen.h"
#include "DolphinNX/Overlay/VulkanOverlay.h"

namespace SwitchFrontend::OverlayRenderer
{
bool Init()
{
  return DolphinNX::LibraryScreen::IsActive() ? DolphinNX::LibraryScreen::RendererInit() :
                                                DolphinNX::VulkanOverlay::RendererInit();
}

void Shutdown()
{
  if (DolphinNX::LibraryScreen::IsActive())
    DolphinNX::LibraryScreen::RendererShutdown();
  else
    DolphinNX::VulkanOverlay::RendererShutdown();
}

void BeginFrame()
{
  if (DolphinNX::LibraryScreen::IsActive())
    DolphinNX::LibraryScreen::RendererBeginFrame();
  else
    DolphinNX::VulkanOverlay::RendererBeginFrame();
}
}  // namespace SwitchFrontend::OverlayRenderer
