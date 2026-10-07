// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

struct FrameGenerationConfig;

namespace VideoCommon
{
class FrameGenerator;
}

namespace Deko3D
{
std::unique_ptr<VideoCommon::FrameGenerator>
CreateFrameGenerator(const FrameGenerationConfig& config);
}  // namespace Deko3D
