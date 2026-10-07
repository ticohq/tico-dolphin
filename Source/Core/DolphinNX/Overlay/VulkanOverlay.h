// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include <switch.h>

#include "overlay/overlay_ui.h"

// tico's overlay (tico/overlay) drawn over Dolphin's Vulkan presentation.
//
// The menu is built and drawn wherever Dolphin presents: the GPU thread while
// the game runs, the main thread while it is paused for the menu. Everything
// the main loop does goes through here, so the two never touch the overlay at
// once.
namespace DolphinNX::VulkanOverlay
{
// Once the swapchain presents: draws the overlay from the next present on.
bool Init();
void Shutdown();

// Plus + Minus opens and closes the menu; while it is open the pad drives it.
void Update(PadState* pad);
bool IsVisible();
void SetVisible(bool visible);

// Opens the menu on a message with one row per choice (e.g. the controller
// modes tip); the choice comes back as Action::NoticeChoice.
void ShowNotice(std::string message, std::vector<std::string> choices);

// Reads the Cheats menu's list again on the next frame (e.g. after a download).
void RequestCheatRefresh();

// The action the menu returned since the last call, once.
SwitchFrontend::OverlayUI::Action ConsumeAction();
}  // namespace DolphinNX::VulkanOverlay
