// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include <switch.h>

#include "overlay/overlay_ui.h"

// tico's overlay (tico/overlay) drawn over Dolphin's presentation, through AbstractGfx
// so it works on any backend (Vulkan, deko3d).
//
// The menu is built and drawn wherever Dolphin presents: the GPU thread while
// the game runs, the main thread while it is paused for the menu. Everything
// the main loop does goes through here, so the two never touch the overlay at
// once.
namespace DolphinNX::GameOverlay
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

// Opens the menu on "Continue where you left off?"; Continue comes back as the
// auto slot's load action, Start Over as Action::Resume.
void ShowResumePrompt();

// A picture (PNG) as a texture for the menu, and its width / height; 0 when there
// is none. Only while the menu is being drawn (e.g. from a slot preview callback).
unsigned long long LoadPicture(const std::string& path, float* aspect);
void FreePicture(unsigned long long texture);

// Reads the Cheats menu's list again on the next frame (e.g. after a download).
void RequestCheatRefresh();

// The overlay renderer over Dolphin's presentation (see OverlayRenderer.cpp).
bool RendererInit();
void RendererShutdown();
void RendererBeginFrame();

// The action the menu returned since the last call, once.
SwitchFrontend::OverlayUI::Action ConsumeAction();
}  // namespace DolphinNX::GameOverlay
