// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <memory>
#include <string>
#include <utility>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

namespace Deko3D
{
// Ring allocator over a single memory block.
class DKStreamBuffer
{
public:
  ~DKStreamBuffer();

  // The name only ever shows up in diagnostics.
  static std::unique_ptr<DKStreamBuffer> Create(u32 size, std::string name);

  DkGpuAddr GetGpuAddr() const { return m_gpu_addr; }
  DkGpuAddr GetCurrentGpuAddr() const { return m_gpu_addr + m_current_offset; }
  DkMemBlock GetMemBlock() const { return m_memblock; }
  u8* GetHostPointer() const { return m_host_pointer; }
  u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  u32 GetCurrentSize() const { return m_size; }
  u32 GetCurrentOffset() const { return m_current_offset; }

  // Returns false when the ring is full, in which case the caller has to submit the command
  // buffer currently being recorded and try again.
  bool ReserveMemory(u32 num_bytes, u32 alignment);
  void CommitMemory(u32 final_num_bytes);

private:
  DKStreamBuffer(u32 size, std::string name);

  bool Allocate();
  void UpdateCurrentFencePosition();
  void UpdateGPUPosition();
  bool WaitForClearSpace(u32 num_bytes);

  u32 m_size;
  std::string m_name;
  u32 m_current_offset = 0;
  u32 m_current_gpu_position = 0;
  u32 m_last_allocation_size = 0;

  dk::UniqueMemBlock m_memblock;
  DkGpuAddr m_gpu_addr = DK_GPU_ADDR_INVALID;
  u8* m_host_pointer = nullptr;

  // Fence counters and the buffer position they correspond to.
  std::deque<std::pair<u64, u32>> m_tracked_fences;
};
}  // namespace Deko3D
