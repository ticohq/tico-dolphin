// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class AbstractTexture;

namespace VideoCommon
{
class FrameGenerator
{
public:
  virtual ~FrameGenerator() = default;

  virtual bool Generate(const AbstractTexture* frame, const MathUtil::Rectangle<int>& rect) = 0;

  virtual const AbstractTexture* GetGeneratedFrame(u32 index) const = 0;
  virtual u32 GetGeneratedFrameCount() const = 0;

  virtual DT GetDisplayInterval() const = 0;

  virtual void Reset() = 0;
};
}  // namespace VideoCommon
