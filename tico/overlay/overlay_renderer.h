// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The renderer the overlay draws with, provided by the frontend: Dolphin's
// Vulkan presenter (DolphinNX/Overlay/VulkanOverlay.cpp).
namespace SwitchFrontend::OverlayRenderer {

// Sets up the ImGui Vulkan backend for the current ImGui context.
bool Init();
void Shutdown();
// Before ImGui::NewFrame.
void BeginFrame();

} // namespace SwitchFrontend::OverlayRenderer
