// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <switch.h>

namespace DolphinNX::VulkanOverlay
{

bool Init();
void Update(PadState* pad);

bool IsVisible();
bool ShouldExit();

int ConsumeAction();

void Shutdown();

}  // namespace DolphinNX::VulkanOverlay
