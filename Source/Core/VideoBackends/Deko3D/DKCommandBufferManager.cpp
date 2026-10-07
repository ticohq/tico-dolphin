// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"

#include <algorithm>
#include <utility>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKSwapChain.h"
#include "VideoCommon/Statistics.h"

namespace Deko3D
{
std::unique_ptr<DKCommandBufferManager> g_dk_command_buffer_mgr;

void DKCommandBufferManager::CommandMemory::Rewind()
{
  // dkCmdBufClear only rewinds to the start of the most recently added slice, so the initial one
  // has to be re-added to get back to the beginning.
  cmdbuf.clear();
  cmdbuf.addMemory(initial_block, initial_offset, initial_size);
  next_growth_chunk = 0;
}

void DKCommandBufferManager::CommandMemory::Grow(size_t min_req_size)
{
  const u32 size = static_cast<u32>(Common::AlignUp(
      std::max<size_t>(min_req_size, COMMAND_BUFFER_GROWTH_SIZE), DK_MEMBLOCK_ALIGNMENT));

  if (next_growth_chunk == growth_chunks.size())
    growth_chunks.push_back(std::make_unique<GrowthChunk>());

  GrowthChunk& chunk = *growth_chunks[next_growth_chunk];
  if (!chunk.block || chunk.size < size)
  {
    // A command buffer is only rewound after the GPU has finished with everything it
    // previously recorded, so nothing still references the old chunk.
    chunk.block = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
                      .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                      .create();
    chunk.size = size;
    MemoryTracker::RegisterMemBlock(chunk.block, "command memory growth chunk");
  }

  cmdbuf.addMemory(chunk.block, 0, chunk.size);
  ++next_growth_chunk;
}

DKCommandBufferManager::DKCommandBufferManager() = default;

DKCommandBufferManager::~DKCommandBufferManager()
{
  if (g_dk_context)
    g_dk_context->WaitIdle();

  for (CmdBufferResources& resources : m_command_buffers)
  {
    for (auto& cleanup : resources.cleanup_resources)
      cleanup();
    resources.cleanup_resources.clear();
  }
}

bool DKCommandBufferManager::Initialize()
{
  if (!CreateCommandBuffers())
    return false;

  CreateTimestampMemory();

  // Give the first command buffer a counter so it can be recorded into straight away.
  m_command_buffers[0].fence_counter = m_next_fence_counter++;
  WriteBeginTimestamp(0);
  return true;
}

bool DKCommandBufferManager::CreateTimestampMemory()
{
  const u32 size = static_cast<u32>(
      Common::AlignUp(sizeof(TimestampReport) * 2 * NUM_COMMAND_BUFFERS, DK_MEMBLOCK_ALIGNMENT));

  m_timestamp_memory = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
                           .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached)
                           .create();
  if (!m_timestamp_memory)
  {
    WARN_LOG_FMT(VIDEO, "deko3d: no memory for GPU timestamps");
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_timestamp_memory, "GPU timestamps");
  m_timestamps = static_cast<const TimestampReport*>(m_timestamp_memory.getCpuAddr());
  return true;
}

void DKCommandBufferManager::WriteBeginTimestamp(u32 index)
{
  if (!m_timestamp_memory)
    return;

  dkCmdBufReportCounter(m_command_buffers[index].draw.cmdbuf, DkCounter_TimestampPipelineTop,
                        m_timestamp_memory.getGpuAddr() + index * 2 * sizeof(TimestampReport));
  m_command_buffers[index].timestamps_written = false;
}

void DKCommandBufferManager::WriteEndTimestamp(u32 index)
{
  if (!m_timestamp_memory)
    return;

  dkCmdBufReportCounter(m_command_buffers[index].draw.cmdbuf, DkCounter_Timestamp,
                        m_timestamp_memory.getGpuAddr() +
                            (index * 2 + 1) * sizeof(TimestampReport));
  m_command_buffers[index].timestamps_written = true;
}

void DKCommandBufferManager::CollectTimestamps(CmdBufferResources& resources, u32 index)
{
  if (!resources.timestamps_written)
    return;
  resources.timestamps_written = false;

  const u64 begin = m_timestamps[index * 2].timestamp;
  const u64 end = m_timestamps[index * 2 + 1].timestamp;
  if (end <= begin)
    return;

  const u64 busy_ns = dkTimestampToNs(end - begin);
  m_gpu_time_ns_since_present += busy_ns;
  g_stats.gpu_busy_ns_total.fetch_add(busy_ns, std::memory_order_relaxed);
}

void DKCommandBufferManager::PublishFrameGpuTime()
{
  g_stats.gpu_frame_time_ms = static_cast<float>(m_gpu_time_ns_since_present) / 1.0e6f;
  m_gpu_time_ns_since_present = 0;
  g_stats.presents_total.fetch_add(1, std::memory_order_relaxed);
}

void DKCommandBufferManager::AddMemoryCallback(void* user_data, DkCmdBuf /*cmdbuf*/,
                                               size_t min_req_size)
{
  static_cast<CommandMemory*>(user_data)->Grow(min_req_size);
}

bool DKCommandBufferManager::CreateCommandBuffers()
{
  DkDevice device = g_dk_context->GetDevice();

  const u32 total_size = static_cast<u32>(Common::AlignUp(
      static_cast<u64>(INIT_COMMAND_BUFFER_SIZE + DRAW_COMMAND_BUFFER_SIZE) * NUM_COMMAND_BUFFERS,
      DK_MEMBLOCK_ALIGNMENT));

  m_command_memory = dk::MemBlockMaker{device, total_size}
                         .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                         .create();
  if (!m_command_memory)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate {} bytes of command memory", total_size);
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_command_memory, "command memory");

  u32 offset = 0;
  const auto create = [&](CommandMemory& mem, u32 size) {
    mem.cmdbuf = dk::CmdBufMaker{device}
                     .setUserData(&mem)
                     .setCbAddMem(&DKCommandBufferManager::AddMemoryCallback)
                     .create();
    if (!mem.cmdbuf)
      return false;

    mem.initial_block = m_command_memory;
    mem.initial_offset = offset;
    mem.initial_size = size;
    mem.cmdbuf.addMemory(m_command_memory, offset, size);
    offset += size;
    return true;
  };

  for (CmdBufferResources& resources : m_command_buffers)
  {
    if (!create(resources.init, INIT_COMMAND_BUFFER_SIZE) ||
        !create(resources.draw, DRAW_COMMAND_BUFFER_SIZE))
    {
      ERROR_LOG_FMT(VIDEO, "deko3d: failed to create command buffer");
      return false;
    }
  }

  return true;
}

DkCmdBuf DKCommandBufferManager::GetCurrentInitCommandBuffer()
{
  CmdBufferResources& resources = m_command_buffers[m_current_cmd_buffer];
  resources.init_cmdbuf_used = true;
  return resources.init.cmdbuf;
}

DkCmdBuf DKCommandBufferManager::GetCurrentCommandBuffer() const
{
  return m_command_buffers[m_current_cmd_buffer].draw.cmdbuf;
}

u64 DKCommandBufferManager::GetCurrentFenceCounter() const
{
  return m_command_buffers[m_current_cmd_buffer].fence_counter;
}

void DKCommandBufferManager::DeferCleanup(std::function<void()> cleanup)
{
  m_command_buffers[m_current_cmd_buffer].cleanup_resources.push_back(std::move(cleanup));
}

void DKCommandBufferManager::NotifyCpuReadback()
{
  m_command_buffers[m_current_cmd_buffer].needs_cpu_readback = true;
}

void DKCommandBufferManager::SubmitCommandBuffer(bool wait_for_completion,
                                                 DKSwapChain* present_swap_chain, int present_slot)
{
  CmdBufferResources& resources = m_command_buffers[m_current_cmd_buffer];
  dk::Queue queue = g_dk_context->GetGraphicsQueue();

  // Uploads recorded this frame have to land before the draws that read them.
  if (resources.init_cmdbuf_used)
    queue.submitCommands(resources.init.cmdbuf.finishList());

  WriteEndTimestamp(m_current_cmd_buffer);
  queue.submitCommands(resources.draw.cmdbuf.finishList());

  queue.signalFence(resources.fence, resources.needs_cpu_readback);

  // Submitted work does not begin executing until the queue is flushed.
  if (present_swap_chain)
    present_swap_chain->Present(present_slot);
  else
    queue.flush();

  if (wait_for_completion)
    WaitForCommandBufferCompletion(m_current_cmd_buffer);

  BeginCommandBuffer();
}

void DKCommandBufferManager::BeginCommandBuffer()
{
  const u32 next_buffer_index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  CmdBufferResources& resources = m_command_buffers[next_buffer_index];

  // Wait for the GPU to finish with everything the memory we are about to reuse still backs.
  if (resources.fence_counter > m_completed_fence_counter)
    WaitForCommandBufferCompletion(next_buffer_index);

  resources.init.Rewind();
  resources.draw.Rewind();
  resources.init_cmdbuf_used = false;
  resources.needs_cpu_readback = false;
  resources.fence_counter = m_next_fence_counter++;
  m_current_cmd_buffer = next_buffer_index;
  WriteBeginTimestamp(next_buffer_index);
}

void DKCommandBufferManager::WaitForCommandBufferCompletion(u32 index)
{
  CmdBufferResources& resources = m_command_buffers[index];

  const DkResult res = dkFenceWait(&resources.fence, -1);
  if (res != DkResult_Success)
    ERROR_LOG_FMT(VIDEO, "deko3d: dkFenceWait failed ({})", static_cast<int>(res));

  const u64 now_completed_counter = resources.fence_counter;
  for (u32 retired_index = 0; retired_index < NUM_COMMAND_BUFFERS; ++retired_index)
  {
    CmdBufferResources& retired = m_command_buffers[retired_index];
    if (retired.fence_counter > now_completed_counter)
      continue;

    CollectTimestamps(retired, retired_index);
    for (auto& cleanup : retired.cleanup_resources)
      cleanup();
    retired.cleanup_resources.clear();
  }

  m_completed_fence_counter = now_completed_counter;
}

void DKCommandBufferManager::WaitForFenceCounter(u64 fence_counter)
{
  if (m_completed_fence_counter >= fence_counter)
    return;

  if (fence_counter >= m_command_buffers[m_current_cmd_buffer].fence_counter)
  {
    SubmitCommandBuffer(true);
    return;
  }

  // Find the first command buffer that covers the counter we are waiting for.
  u32 index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  while (index != m_current_cmd_buffer)
  {
    if (m_command_buffers[index].fence_counter >= fence_counter)
      break;

    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }

  ASSERT(index != m_current_cmd_buffer);
  WaitForCommandBufferCompletion(index);
}

void DeferMemBlockDestruction(dk::UniqueMemBlock block)
{
  if (!block)
    return;

  if (g_dk_command_buffer_mgr)
  {
    // std::function requires a copyable callable, so ownership passes through a shared_ptr the
    // lambda keeps alive until the cleanup list is cleared.
    auto shared = std::make_shared<dk::UniqueMemBlock>(std::move(block));
    g_dk_command_buffer_mgr->DeferCleanup([shared]() {});
  }
  // Otherwise the GPU has already been drained and the block is freed here.
}
}  // namespace Deko3D
