// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

namespace Deko3D
{
// Owns the deko3d device and the main graphics/compute queue.
class DKContext
{
public:
  ~DKContext();

  // Creates the device and main queue. Returns null on failure.
  static std::unique_ptr<DKContext> Create();

  DkDevice GetDevice() const { return m_device; }
  dk::Queue GetGraphicsQueue() const { return m_queue; }

  // Waits for the queue to drain.
  void WaitIdle();

private:
  DKContext() = default;
  bool Initialize();

  static void DebugCallback(void* user_data, const char* context, DkResult result,
                            const char* message);

  dk::UniqueDevice m_device;
  dk::UniqueQueue m_queue;
};

extern std::unique_ptr<DKContext> g_dk_context;
}  // namespace Deko3D
