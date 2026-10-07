// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The renderer the overlay draws with, provided by the frontend: Dolphin's
// presentation, through AbstractGfx (DolphinNX/Overlay/GameOverlay.cpp).
namespace SwitchFrontend::OverlayRenderer {

// Sets up the renderer for the current ImGui context.
bool Init();
void Shutdown();
// Before ImGui::NewFrame.
void BeginFrame();

} // namespace SwitchFrontend::OverlayRenderer
