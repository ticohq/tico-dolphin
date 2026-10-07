// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "overlay/overlay_ui.h"

struct ImDrawData;
class IOverlayHost;

namespace SwitchFrontend::ImGuiOverlay {

// Creates the ImGui context, fonts and the Vulkan overlay renderer, and
// uploads the avatar and selection border through the host.
bool Init(IOverlayHost* host);
void Shutdown();

// Shows or hides the quick menu (the HUD and toasts are drawn either way).
void SetVisible(bool visible);
bool IsVisible();

// Edge-triggered menu navigation for the next built frame.
void FeedNav(const OverlayUI::NavInput& nav);
// The touchscreen's current state, read while the menu is open.
void FeedTouch(const OverlayUI::TouchInput& touch);

// Builds this frame's overlay for a width x height surface. Returns the draw
// data to composite over the game.
ImDrawData* BuildFrame(float width, float height, float delta_time);

// The action the menu returned while building, once; None when there was none.
OverlayUI::Action ConsumeAction();

} // namespace SwitchFrontend::ImGuiOverlay
