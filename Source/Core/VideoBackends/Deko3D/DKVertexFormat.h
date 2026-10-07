// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoBackends/Deko3D/Constants.h"
#include "VideoCommon/NativeVertexFormat.h"

namespace Deko3D
{
// Translates a PortableVertexDeclaration into the deko3d attribute/buffer state that a pipeline
// bakes into its replay commands.
class DKVertexFormat final : public ::NativeVertexFormat
{
public:
  explicit DKVertexFormat(const PortableVertexDeclaration& vtx_decl);

  // The full 16-slot array is bound so a pipeline's replay always establishes a complete,
  // deterministic attribute configuration rather than leaving stale slots from a prior pipeline.
  const std::array<DkVtxAttribState, MAX_VERTEX_ATTRIBUTES>& GetAttributes() const
  {
    return m_attributes;
  }
  const DkVtxBufferState& GetBufferState() const { return m_buffer_state; }

  static const std::array<DkVtxAttribState, MAX_VERTEX_ATTRIBUTES>& GetUnusedAttributes();

private:
  void MapAttributes();
  void AddAttribute(u32 location, DkVtxAttribSize size, DkVtxAttribType type, u32 offset);

  // Locations MapAttributes leaves alone hold deko3d's inactive encoding, not zeroes.
  // Zeroes cause crashes and crashes aren't fun.
  std::array<DkVtxAttribState, MAX_VERTEX_ATTRIBUTES> m_attributes{};
  DkVtxBufferState m_buffer_state{};
};
}  // namespace Deko3D
