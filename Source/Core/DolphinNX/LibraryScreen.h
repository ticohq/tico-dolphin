// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

// The game list, for when Dolphin is started without a game (from the homebrew
// menu): tico's overlay in library mode, drawn through SDL before any of Dolphin
// is up.
namespace DolphinNX::LibraryScreen
{
// Shows the list until a game is chosen (its path) or the player leaves (nullopt).
std::optional<std::string> Run();
bool IsActive();

// The overlay renderer while it runs (see Overlay/OverlayRenderer.cpp).
bool RendererInit();
void RendererShutdown();
void RendererBeginFrame();
}  // namespace DolphinNX::LibraryScreen
