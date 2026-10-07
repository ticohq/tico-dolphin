// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKVertexFormat.h"

#include "Common/Assert.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexShaderGen.h"

namespace Deko3D
{
namespace
{
struct DkAttribFormat
{
  DkVtxAttribSize size;
  DkVtxAttribType type;
};

DkAttribFormat VarToDkFormat(ComponentFormat t, u32 components, bool integer)
{
  ASSERT(components >= 1 && components <= 4);
  const u32 idx = components - 1;

  static constexpr std::array<DkVtxAttribSize, 4> sizes_8 = {
      DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8};
  static constexpr std::array<DkVtxAttribSize, 4> sizes_16 = {
      DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16};
  static constexpr std::array<DkVtxAttribSize, 4> sizes_32 = {
      DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};

  switch (t)
  {
  case ComponentFormat::UByte:
    return {sizes_8[idx], integer ? DkVtxAttribType_Uint : DkVtxAttribType_Unorm};
  case ComponentFormat::Byte:
    return {sizes_8[idx], integer ? DkVtxAttribType_Sint : DkVtxAttribType_Snorm};
  case ComponentFormat::UShort:
    return {sizes_16[idx], integer ? DkVtxAttribType_Uint : DkVtxAttribType_Unorm};
  case ComponentFormat::Short:
    return {sizes_16[idx], integer ? DkVtxAttribType_Sint : DkVtxAttribType_Snorm};
  case ComponentFormat::Float:
  default:
    // The Invalid* formats are assumed to behave like float.
    return {sizes_32[idx], DkVtxAttribType_Float};
  }
}

// deko3d pads the attribute slots past the ones it is handed with this, and only those.
DkVtxAttribState MakeUnusedAttribute()
{
  DkVtxAttribState attr{};
  attr.isFixed = 1;
  attr.size = DkVtxAttribSize_1x32;
  attr.type = DkVtxAttribType_Float;
  return attr;
}
}  // namespace

DKVertexFormat::DKVertexFormat(const PortableVertexDeclaration& vtx_decl)
    : NativeVertexFormat(vtx_decl)
{
  m_buffer_state.stride = static_cast<u32>(m_decl.stride);
  m_buffer_state.divisor = 0;
  m_attributes.fill(MakeUnusedAttribute());
  MapAttributes();
}

const std::array<DkVtxAttribState, MAX_VERTEX_ATTRIBUTES>& DKVertexFormat::GetUnusedAttributes()
{
  static const auto attributes = [] {
    std::array<DkVtxAttribState, MAX_VERTEX_ATTRIBUTES> unused;
    unused.fill(MakeUnusedAttribute());
    return unused;
  }();
  return attributes;
}

void DKVertexFormat::AddAttribute(u32 location, DkVtxAttribSize size, DkVtxAttribType type,
                                  u32 offset)
{
  ASSERT(location < MAX_VERTEX_ATTRIBUTES);

  DkVtxAttribState& attr = m_attributes[location];
  attr.bufferId = 0;
  attr.isFixed = 0;
  attr.offset = offset;
  attr.size = size;
  attr.type = type;
  attr.isBgra = 0;
}

void DKVertexFormat::MapAttributes()
{
  const auto add = [this](ShaderAttrib location, const AttributeFormat& format) {
    const DkAttribFormat dk =
        VarToDkFormat(format.type, static_cast<u32>(format.components), format.integer);
    AddAttribute(static_cast<u32>(location), dk.size, dk.type, static_cast<u32>(format.offset));
  };

  if (m_decl.position.enable)
    add(ShaderAttrib::Position, m_decl.position);

  for (u32 i = 0; i < m_decl.normals.size(); i++)
  {
    if (m_decl.normals[i].enable)
      add(ShaderAttrib::Normal + i, m_decl.normals[i]);
  }

  for (u32 i = 0; i < m_decl.colors.size(); i++)
  {
    if (m_decl.colors[i].enable)
      add(ShaderAttrib::Color0 + i, m_decl.colors[i]);
  }

  for (u32 i = 0; i < m_decl.texcoords.size(); i++)
  {
    if (m_decl.texcoords[i].enable)
      add(ShaderAttrib::TexCoord0 + i, m_decl.texcoords[i]);
  }

  if (m_decl.posmtx.enable)
    add(ShaderAttrib::PositionMatrix, m_decl.posmtx);
}
}  // namespace Deko3D
