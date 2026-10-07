// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractShader.h"

namespace Deko3D
{
// The GPU fetches shader code from this block at draw time, but the only thing a pipeline keeps is
// its address.
// videocommon frees an AbstractShader as soon as CreatePipeline returns, so the code
// has to be reference counted and outlive both the DKShader and every pipeline built from it.
class DKShaderCode
{
public:
  DKShaderCode(dk::UniqueMemBlock block, const DkShader& shader);
  ~DKShaderCode();

  const DkShader* GetShader() const { return &m_shader; }
  bool IsValid() const { return dkShaderIsValid(&m_shader); }

private:
  dk::UniqueMemBlock m_block;
  DkShader m_shader;
};

// Wraps a DkShader plus the code DkMemBlock it lives in.
class DKShader final : public AbstractShader
{
public:
  DKShader(ShaderStage stage, std::vector<u8> dksh, std::shared_ptr<DKShaderCode> code);
  ~DKShader() override;

  // The pipeline bakes this into its captured bind commands.
  const DkShader* GetShader() const { return m_code->GetShader(); }
  bool IsValid() const { return m_code && m_code->IsValid(); }

  // Taken by anything that records the code's address into a command buffer.
  const std::shared_ptr<DKShaderCode>& GetCode() const { return m_code; }

  // Returns a versioned DKSH blob for the disk shader cache.
  BinaryData GetBinary() const override;

  static std::unique_ptr<DKShader> CreateFromBinary(ShaderStage stage, const void* data,
                                                    size_t length, std::string_view name);
  static std::unique_ptr<DKShader> CreateFromCacheBinary(ShaderStage stage, const void* data,
                                                         size_t length, std::string_view name);

private:
  std::vector<u8> m_dksh;
  std::shared_ptr<DKShaderCode> m_code;
};
}  // namespace Deko3D
