// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKShaderCompiler.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Common/Timer.h"

#include "VideoBackends/Deko3D/Constants.h"
#include "VideoBackends/Deko3D/UamBridge.h"

#include "VideoCommon/ShaderCompileUtils.h"
#include "VideoCommon/Spirv.h"

namespace Deko3D::ShaderCompiler
{
namespace
{
constexpr char GLSL_VERSION[] = "#version 450 core\n";

constexpr char INCLUDE_EXTENSION[] = "#extension GL_ARB_shading_language_include : enable\n";

// The Vulkan backend's header with the descriptor sets removed.
//
// INPUT_ATTACHMENT_BINDING is deliberately left undefined.
//
// gl_VertexID/gl_InstanceID keep their GL names
static_assert(TEXEL_BUFFER_BINDING_BASE == 16, "TEXEL_BUFFER_BINDING below is out of date");
constexpr char SHADER_HEADER[] = R"(
  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = (x + 16))
  #define SSBO_BINDING(x) layout(std430, binding = x)
  #define VARYING_LOCATION(x) layout(location = x)
  #define FORCE_EARLY_Z layout(early_fragment_tests) in

  // hlsl to glsl function translation
  #define API_VULKAN 1
  #define float2 vec2
  #define float3 vec3
  #define float4 vec4
  #define uint2 uvec2
  #define uint3 uvec3
  #define uint4 uvec4
  #define int2 ivec2
  #define int3 ivec3
  #define int4 ivec4
  #define frac fract
  #define lerp mix
)";

constexpr char COMPUTE_SHADER_HEADER[] = R"(
  #define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = (x + 16))
  #define IMAGE_BINDING(format, x) layout(format, binding = x)

  #define API_VULKAN 1
  #define float2 vec2
  #define float3 vec3
  #define float4 vec4
  #define uint2 uvec2
  #define uint3 uvec3
  #define uint4 uvec4
  #define int2 ivec2
  #define int3 ivec3
  #define int4 ivec4
  #define frac fract
  #define lerp mix
)";

int StageToUam(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:
    return UamStage_Vertex;
  case ShaderStage::Geometry:
    return UamStage_Geometry;
  case ShaderStage::Pixel:
    return UamStage_Fragment;
  case ShaderStage::Compute:
    return UamStage_Compute;
  default:
    return UamStage_Vertex;
  }
}

EShLanguage StageToGlslang(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:
    return EShLangVertex;
  case ShaderStage::Geometry:
    return EShLangGeometry;
  case ShaderStage::Pixel:
    return EShLangFragment;
  case ShaderStage::Compute:
    return EShLangCompute;
  default:
    return EShLangVertex;
  }
}

// uam has no include support
std::optional<std::string> FlattenIncludes(ShaderStage stage, std::string_view source,
                                           VideoCommon::ShaderIncluder* shader_includer)
{
  const auto preprocessed = SPIRV::PreprocessShader(StageToGlslang(stage), source, shader_includer);
  if (!preprocessed)
    return std::nullopt;

  std::string flattened;
  flattened.reserve(preprocessed->size());
  std::string_view remaining = *preprocessed;
  while (!remaining.empty())
  {
    const size_t line_end = remaining.find('\n');
    const std::string_view line = remaining.substr(0, line_end);
    remaining =
        line_end == std::string_view::npos ? std::string_view{} : remaining.substr(line_end + 1);

    const size_t first = line.find_first_not_of(" \t");
    const std::string_view directive =
        first == std::string_view::npos ? std::string_view{} : line.substr(first);
    if (directive.starts_with("#line") ||
        (directive.starts_with("#extension") &&
         (directive.find("GL_ARB_shading_language_include") != std::string_view::npos ||
          directive.find("GL_GOOGLE_include_directive") != std::string_view::npos)))
    {
      continue;
    }

    flattened.append(line);
    flattened.push_back('\n');
  }

  return flattened;
}

// uam is mesa 19.0's single-threaded standalone compiler.
std::mutex s_compile_mutex;
}  // namespace

std::optional<std::vector<u8>> CompileShader(ShaderStage stage, std::string_view source,
                                             VideoCommon::ShaderIncluder* shader_includer,
                                             std::string_view name)
{
  const std::string_view header =
      stage == ShaderStage::Compute ? COMPUTE_SHADER_HEADER : SHADER_HEADER;

  const bool has_includes =
      shader_includer != nullptr && source.find("#include") != std::string_view::npos;

  std::string full_source;
  full_source.reserve(std::size(GLSL_VERSION) + std::size(INCLUDE_EXTENSION) + header.size() +
                      source.size());
  full_source.append(GLSL_VERSION);
  if (has_includes)
    full_source.append(INCLUDE_EXTENSION);
  full_source.append(header);
  full_source.append(source);

  if (has_includes)
  {
    auto flattened = FlattenIncludes(stage, full_source, shader_includer);
    if (!flattened)
    {
      ERROR_LOG_FMT(VIDEO, "deko3d: could not resolve the includes of '{}'", name);
      return std::nullopt;
    }
    full_source = std::move(*flattened);
  }

  Common::Timer timer;
  timer.Start();

  size_t dksh_size = 0;
  char* log = nullptr;
  void* dksh = nullptr;
  {
    std::lock_guard guard(s_compile_mutex);
    dksh = UamCompileGlsl(full_source.c_str(), StageToUam(stage), &dksh_size, &log);
  }

  const bool have_log = log != nullptr && log[0] != '\0';
  if (!dksh)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: uam rejected '{}':\n{}", name,
                  have_log ? log : "(no diagnostics)");
    std::free(log);
    return std::nullopt;
  }

  if (have_log)
    WARN_LOG_FMT(VIDEO, "deko3d: uam warnings for '{}':\n{}", name, log);
  std::free(log);

  DEBUG_LOG_FMT(VIDEO, "deko3d: compiled '{}' to {} bytes of DKSH in {} ms", name, dksh_size,
                timer.ElapsedMs());

  const auto* bytes = static_cast<const u8*>(dksh);
  std::vector<u8> blob(bytes, bytes + dksh_size);
  std::free(dksh);
  return blob;
}
}  // namespace Deko3D::ShaderCompiler
