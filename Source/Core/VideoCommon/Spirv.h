// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glslang/Public/ShaderLang.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/VideoCommon.h"

namespace SPIRV
{
// SPIR-V compiled code type
using CodeType = u32;
using CodeVector = std::vector<CodeType>;

// Compile a vertex shader to SPIR-V.
std::optional<CodeVector> CompileVertexShader(std::string_view source_code, APIType api_type,
                                              glslang::EShTargetLanguageVersion language_version,
                                              glslang::TShader::Includer* shader_includer);

// Compile a geometry shader to SPIR-V.
std::optional<CodeVector> CompileGeometryShader(std::string_view source_code, APIType api_type,
                                                glslang::EShTargetLanguageVersion language_version,
                                                glslang::TShader::Includer* shader_includer);

// Compile a fragment shader to SPIR-V.
std::optional<CodeVector> CompileFragmentShader(std::string_view source_code, APIType api_type,
                                                glslang::EShTargetLanguageVersion language_version,
                                                glslang::TShader::Includer* shader_includer);

// Compile a compute shader to SPIR-V.
std::optional<CodeVector> CompileComputeShader(std::string_view source_code, APIType api_type,
                                               glslang::EShTargetLanguageVersion language_version,
                                               glslang::TShader::Includer* shader_includer);

// For compilers that cannot resolve #include directives themselves (deko3d's uam).
std::optional<std::string> PreprocessShader(EShLanguage stage, std::string_view source_code,
                                            glslang::TShader::Includer* shader_includer);
}  // namespace SPIRV
