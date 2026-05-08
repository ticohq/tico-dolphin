// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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

}  // namespace Input
}  // namespace DolphinNX
