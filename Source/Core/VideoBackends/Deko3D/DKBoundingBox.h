// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoBackends/Deko3D/Constants.h"

#include "VideoCommon/BoundingBox.h"

namespace Deko3D
{
class DKStreamBuffer;

class DKBoundingBox final : public BoundingBox
{
public:
  DKBoundingBox();
  ~DKBoundingBox() override;

  bool Initialize() override;

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, std::span<const BBoxType> values) override;

private:
  static constexpr u32 BUFFER_SIZE = sizeof(BBoxType) * NUM_BBOX_VALUES;
  static constexpr u32 MAX_UPDATES_PER_FRAME = 128;
  static constexpr u32 UPLOAD_BUFFER_SIZE =
      BUFFER_SIZE * MAX_UPDATES_PER_FRAME * NUM_COMMAND_BUFFERS;

  dk::UniqueMemBlock m_gpu_buffer;
  dk::UniqueMemBlock m_readback_buffer;
  std::unique_ptr<DKStreamBuffer> m_upload_buffer;
  BBoxType* m_readback_pointer = nullptr;
};
}  // namespace Deko3D
