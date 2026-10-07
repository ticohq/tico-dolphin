// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

// The renderer tico's overlay draws with: SDL's while the game list runs before a
// game (LibraryScreen), Dolphin's presentation during a game (GameOverlay).

#include "overlay/overlay_renderer.h"

#include "DolphinNX/LibraryScreen.h"
#include "DolphinNX/Overlay/GameOverlay.h"

namespace SwitchFrontend::OverlayRenderer
{
bool Init()
{
  return DolphinNX::LibraryScreen::IsActive() ? DolphinNX::LibraryScreen::RendererInit() :
                                                DolphinNX::GameOverlay::RendererInit();
}

void Shutdown()
{
  if (DolphinNX::LibraryScreen::IsActive())
    DolphinNX::LibraryScreen::RendererShutdown();
  else
    DolphinNX::GameOverlay::RendererShutdown();
}

void BeginFrame()
{
  if (DolphinNX::LibraryScreen::IsActive())
    DolphinNX::LibraryScreen::RendererBeginFrame();
  else
    DolphinNX::GameOverlay::RendererBeginFrame();
}
}  // namespace SwitchFrontend::OverlayRenderer
