// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKShader.h"

#include <cstring>

#include "Common/Align.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Deko3D/Constants.h"
#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"

namespace Deko3D
{
namespace
{
constexpr u32 DKSH_MAGIC = 0x48534B44;        // 'DKSH'
constexpr u32 DKSH_CACHE_MAGIC = 0x434B4444;  // 'DDKC'
constexpr u32 DKSH_CACHE_VERSION = 1;

// Header at the start of every DKSH blob.
struct DkshHeader
{
  u32 magic;
  u32 header_sz;
  u32 control_sz;
  u32 code_sz;
  u32 programs_off;
  u32 num_programs;
};

// Keep a backend version beside each cached binary so compiler changes cannot
// silently reuse old Maxwell code.
struct DkshCacheFooter
{
  u32 magic;
  u32 version;
};

// Leading fields of deko3d's DkshProgramHeader.
struct DkshProgramHeader
{
  u32 type;
  u32 entrypoint;
  u32 num_gprs;
  u32 constbuf1_off;
  u32 constbuf1_sz;
  u32 per_warp_scratch_sz;
};
constexpr u32 DKSH_PROGRAM_HEADER_SIZE = 64;

bool ScratchMemoryFits(const u8* control, const DkshHeader& header, std::string_view name)
{
  if (header.programs_off > header.control_sz ||
      (header.control_sz - header.programs_off) / DKSH_PROGRAM_HEADER_SIZE < header.num_programs)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: '{}' has a truncated DKSH program table", name);
    return false;
  }

  for (u32 i = 0; i < header.num_programs; i++)
  {
    DkshProgramHeader program;
    std::memcpy(&program, control + header.programs_off + i * DKSH_PROGRAM_HEADER_SIZE,
                sizeof(program));
    if (program.per_warp_scratch_sz > PER_WARP_SCRATCH_MEMORY_SIZE)
    {
      ERROR_LOG_FMT(VIDEO,
                    "deko3d: '{}' spills {} bytes per warp, more than the {} the queue reserves",
                    name, program.per_warp_scratch_sz, PER_WARP_SCRATCH_MEMORY_SIZE);
      return false;
    }
  }

  return true;
}
}  // namespace

DKShaderCode::DKShaderCode(dk::UniqueMemBlock block, const DkShader& shader)
    : m_block(std::move(block)), m_shader(shader)
{
}

DKShaderCode::~DKShaderCode()
{
  MemoryTracker::UnregisterMemBlock(m_block);
  DeferMemBlockDestruction(std::move(m_block));
}

DKShader::DKShader(ShaderStage stage, std::vector<u8> dksh, std::shared_ptr<DKShaderCode> code)
    : AbstractShader(stage), m_dksh(std::move(dksh)), m_code(std::move(code))
{
}

DKShader::~DKShader() = default;

AbstractShader::BinaryData DKShader::GetBinary() const
{
  BinaryData binary = m_dksh;
  const DkshCacheFooter footer{DKSH_CACHE_MAGIC, DKSH_CACHE_VERSION};
  const u8* footer_bytes = reinterpret_cast<const u8*>(&footer);
  binary.insert(binary.end(), footer_bytes, footer_bytes + sizeof(footer));
  return binary;
}

std::unique_ptr<DKShader> DKShader::CreateFromBinary(ShaderStage stage, const void* data,
                                                     size_t length, std::string_view name)
{
  if (length < sizeof(DkshHeader))
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: DKSH blob for '{}' is too small ({} bytes)", name, length);
    return nullptr;
  }

  std::vector<u8> dksh(length);
  std::memcpy(dksh.data(), data, length);

  DkshHeader header;
  std::memcpy(&header, dksh.data(), sizeof(header));
  if (header.magic != DKSH_MAGIC ||
      static_cast<size_t>(header.control_sz) + header.code_sz > length)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: '{}' is not a valid DKSH blob", name);
    return nullptr;
  }

  if (!ScratchMemoryFits(dksh.data(), header, name))
    return nullptr;

  // The last DK_SHADER_CODE_UNUSABLE_SIZE bytes of any code block cannot hold shader code,
  // so pad the allocation past the code section.
  const u32 block_size = static_cast<u32>(
      Common::AlignUp(header.code_sz + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));

  // TODO: recycle a shared code-segment allocator instead of one block per shader.
  dk::UniqueMemBlock code_block =
      dk::MemBlockMaker{g_dk_context->GetDevice(), block_size}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
          .create();
  if (!code_block)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate {} bytes of shader code memory", block_size);
    return nullptr;
  }

  MemoryTracker::RegisterMemBlock(code_block, fmt::format("shader code '{}'", name));

  // The code section follows the control section in the blob. Only it is copied to GPU code
  // memory. Deko3d reads the control section from the CPU-side copy during initialization.
  std::memcpy(code_block.getCpuAddr(), dksh.data() + header.control_sz, header.code_sz);

  DkShaderMaker maker{};
  dkShaderMakerDefaults(&maker, code_block, 0);
  maker.control = dksh.data();

  DkShader shader{};
  dkShaderInitialize(&shader, &maker);
  if (!dkShaderIsValid(&shader))
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: dkShaderInitialize rejected '{}'", name);
    return nullptr;
  }

  return std::make_unique<DKShader>(stage, std::move(dksh),
                                    std::make_shared<DKShaderCode>(std::move(code_block), shader));
}

std::unique_ptr<DKShader> DKShader::CreateFromCacheBinary(ShaderStage stage, const void* data,
                                                          size_t length, std::string_view name)
{
  if (length < sizeof(DkshCacheFooter))
    return nullptr;

  DkshCacheFooter footer;
  std::memcpy(&footer, static_cast<const u8*>(data) + length - sizeof(footer), sizeof(footer));
  if (footer.magic != DKSH_CACHE_MAGIC || footer.version != DKSH_CACHE_VERSION)
    return nullptr;

  return CreateFromBinary(stage, data, length - sizeof(footer), name);
}
}  // namespace Deko3D
