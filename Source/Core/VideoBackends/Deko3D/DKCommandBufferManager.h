// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoBackends/Deko3D/Constants.h"

namespace Deko3D
{
class DKSwapChain;

// Owns the command buffers in flight and the fences tracking their completion.
class DKCommandBufferManager
{
public:
  DKCommandBufferManager();
  ~DKCommandBufferManager();

  bool Initialize();

  // Upload commands recorded here are submitted ahead of the draw command buffer. Both are only
  // valid until the current command buffer is submitted.
  DkCmdBuf GetCurrentInitCommandBuffer();
  DkCmdBuf GetCurrentCommandBuffer() const;

  // Fence "counters" track which commands the GPU has completed.
  u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }
  u64 GetCurrentFenceCounter() const;

  void WaitForFenceCounter(u64 fence_counter);

  // Makes the fence for the current command buffer flush GPU writes for CPU readback.
  void NotifyCpuReadback();

  // Submits the current command buffer and rotates to the next one.
  void SubmitCommandBuffer(bool wait_for_completion, DKSwapChain* present_swap_chain = nullptr,
                           int present_slot = -1);

  // Runs the callback once the GPU has finished with the command buffer currently being recorded.
  void DeferCleanup(std::function<void()> cleanup);

  // Moves the GPU time collected since the last call into g_stats.
  void PublishFrameGpuTime();

private:
  struct GrowthChunk
  {
    dk::UniqueMemBlock block;
    u32 size = 0;
  };

  // Backing memory for a single DkCmdBuf.
  struct CommandMemory
  {
    dk::UniqueCmdBuf cmdbuf;

    // A slice of the manager's shared command memory block.
    DkMemBlock initial_block = nullptr;
    u32 initial_offset = 0;
    u32 initial_size = 0;

    // Chunks handed out by the growth callback. These are kept and reused across frames rather
    // than freed, so a command buffer that once needed the space does not reallocate every frame.
    std::vector<std::unique_ptr<GrowthChunk>> growth_chunks;
    size_t next_growth_chunk = 0;

    void Rewind();
    void Grow(size_t min_req_size);
  };

  struct CmdBufferResources
  {
    CommandMemory init;
    CommandMemory draw;

    // Zero-initialised so that waiting on it before it has ever been signaled is a no-op.
    DkFence fence = {};
    u64 fence_counter = 0;
    bool init_cmdbuf_used = false;
    bool needs_cpu_readback = false;
    bool timestamps_written = false;

    std::vector<std::function<void()>> cleanup_resources;
  };

  static void AddMemoryCallback(void* user_data, DkCmdBuf cmdbuf, size_t min_req_size);

  bool CreateCommandBuffers();
  void BeginCommandBuffer();
  void WaitForCommandBufferCompletion(u32 index);

  bool CreateTimestampMemory();
  void WriteBeginTimestamp(u32 index);
  void WriteEndTimestamp(u32 index);
  void CollectTimestamps(CmdBufferResources& resources, u32 index);

  dk::UniqueMemBlock m_command_memory;

  u64 m_next_fence_counter = 1;
  u64 m_completed_fence_counter = 0;

  // Recorded command lists reference their fence by address, so this array must not be moved or
  // resized for as long as any of those lists can still be submitted.
  std::array<CmdBufferResources, NUM_COMMAND_BUFFERS> m_command_buffers;
  u32 m_current_cmd_buffer = 0;

  struct TimestampReport
  {
    u64 value;
    u64 timestamp;
  };
  dk::UniqueMemBlock m_timestamp_memory;
  const TimestampReport* m_timestamps = nullptr;
  u64 m_gpu_time_ns_since_present = 0;
};

extern std::unique_ptr<DKCommandBufferManager> g_dk_command_buffer_mgr;

// Frees a memory block once the command buffer currently being recorded has retired.
void DeferMemBlockDestruction(dk::UniqueMemBlock block);
}  // namespace Deko3D
