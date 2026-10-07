// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractShader.h"

namespace VideoCommon
{
class ShaderIncluder;
}

namespace Deko3D::ShaderCompiler
{
// Prepends the deko3d binding preamble to videocommon's generated GLSL and runs it through uam.
std::optional<std::vector<u8>> CompileShader(ShaderStage stage, std::string_view source,
                                             VideoCommon::ShaderIncluder* shader_includer,
                                             std::string_view name);
}  // namespace Deko3D::ShaderCompiler
