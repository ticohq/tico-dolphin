// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

#include "Common/CommonTypes.h"

namespace Deko3D::FrameGeneration
{
enum class ShaderStatus
{
  MissingDll,
  NotPrepared,
  Prepared,
};

std::string GetDirectory();
std::string GetDllPath();

std::string GetCacheDirectory(bool performance);

ShaderStatus GetShaderStatus();

using PrepareProgress = std::function<void(u32 compiled, u32 total)>;

std::string PrepareShaders(const PrepareProgress& progress);
}  // namespace Deko3D::FrameGeneration
