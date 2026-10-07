// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include <switch.h>

#include "Common/WindowSystemInfo.h"

namespace DolphinNX
{
namespace Input
{

void Init(const WindowSystemInfo& wsi);
void Update();
void Shutdown();

PadState* GetPad();

// Settings > Players: per port, the Switch controller connected and what the game
// gets from it ("" when none is connected); a line about switching Wii controller
// modes (empty in GameCube games); and the system's controller screen to
// reconnect or reorder them. Auto profiles follow whatever is then connected.
std::vector<std::string> DescribePorts();
std::string PortsNote();
bool ShowControllerOrder();

// Re-reads the Wii Remote settings that apply while playing (pointer range,
// calibrate on recenter) and applies them to every Wii Remote.
void RefreshLiveSettings();

}  // namespace Input
}  // namespace DolphinNX
