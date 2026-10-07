// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Common.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Deko3D
{
class VideoBackend : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override { return CONFIG_NAME; }
  std::string GetDisplayName() const override { return "deko3d"; }
  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "Deko3D";
};
}  // namespace Deko3D
