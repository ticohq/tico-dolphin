// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKPerfQuery.h"

#include <cstring>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
DKPerfQuery::DKPerfQuery() = default;

DKPerfQuery::~DKPerfQuery() = default;

bool DKPerfQuery::Initialize()
{
  const u32 size = static_cast<u32>(
      Common::AlignUp(sizeof(CounterReport) * PERF_QUERY_BUFFER_SIZE, DK_MEMBLOCK_ALIGNMENT));
  m_query_memblock = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
                         .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                   DkMemBlockFlags_ZeroFillInit)
                         .create();
  if (!m_query_memblock)
  {
    PanicAlertFmt("Failed to allocate the deko3d performance query buffer.");
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_query_memblock, "performance query reports");

  m_query_results = static_cast<CounterReport*>(m_query_memblock.getCpuAddr());
  ResetQuery();
  return true;
}

void DKPerfQuery::EnableQuery(PerfQueryGroup group)
{
  const u32 query_count = m_query_count.load(std::memory_order_relaxed);
  if (query_count > m_query_buffer.size() / 2)
    PartialFlush(query_count == PERF_QUERY_BUFFER_SIZE);

  DKStateTracker::GetInstance()->Bind();

  if (group != PQG_ZCOMP_ZCOMPLOC && group != PQG_ZCOMP)
    return;

  ActiveQuery& entry = m_query_buffer[m_query_next_pos];
  DEBUG_ASSERT(!entry.has_value);
  entry.has_value = true;
  entry.query_group = group;

  dkCmdBufResetCounter(g_dk_command_buffer_mgr->GetCurrentCommandBuffer(), DkCounter_SamplesPassed);
}

void DKPerfQuery::DisableQuery(PerfQueryGroup group)
{
  if (group != PQG_ZCOMP_ZCOMPLOC && group != PQG_ZCOMP)
    return;

  ActiveQuery& entry = m_query_buffer[m_query_next_pos];
  DEBUG_ASSERT(entry.has_value && entry.query_group == group);

  const DkGpuAddr result_addr =
      m_query_memblock.getGpuAddr() + m_query_next_pos * sizeof(CounterReport);
  dkCmdBufReportCounter(g_dk_command_buffer_mgr->GetCurrentCommandBuffer(), DkCounter_SamplesPassed,
                        result_addr);
  g_dk_command_buffer_mgr->NotifyCpuReadback();

  entry.fence_counter = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
  m_query_next_pos = (m_query_next_pos + 1) % PERF_QUERY_BUFFER_SIZE;
  m_query_count.fetch_add(1, std::memory_order_relaxed);
}

void DKPerfQuery::ResetQuery()
{
  m_query_count.store(0, std::memory_order_relaxed);
  m_query_readback_pos = 0;
  m_query_next_pos = 0;
  for (std::atomic<u32>& result : m_results)
    result.store(0, std::memory_order_relaxed);
  m_query_buffer = {};
}

u32 DKPerfQuery::GetQueryResult(PerfQueryType type)
{
  u32 result = 0;
  if (type == PQ_ZCOMP_INPUT_ZCOMPLOC || type == PQ_ZCOMP_OUTPUT_ZCOMPLOC)
  {
    result = m_results[PQG_ZCOMP_ZCOMPLOC].load(std::memory_order_relaxed);
  }
  else if (type == PQ_ZCOMP_INPUT || type == PQ_ZCOMP_OUTPUT)
  {
    result = m_results[PQG_ZCOMP].load(std::memory_order_relaxed);
  }
  else if (type == PQ_BLEND_INPUT)
  {
    result = m_results[PQG_ZCOMP].load(std::memory_order_relaxed) +
             m_results[PQG_ZCOMP_ZCOMPLOC].load(std::memory_order_relaxed);
  }
  else if (type == PQ_EFB_COPY_CLOCKS)
  {
    result = m_results[PQG_EFB_COPY_CLOCKS].load(std::memory_order_relaxed);
  }

  return result / 4;
}

void DKPerfQuery::FlushResults()
{
  if (!IsFlushed())
    PartialFlush(true);

  ASSERT(IsFlushed());
}

bool DKPerfQuery::IsFlushed() const
{
  return m_query_count.load(std::memory_order_relaxed) == 0;
}

void DKPerfQuery::ReadbackQueries()
{
  const u64 completed_fence_counter = g_dk_command_buffer_mgr->GetCompletedFenceCounter();
  const u32 outstanding_queries = m_query_count.load(std::memory_order_relaxed);
  u32 readback_count = 0;

  for (; readback_count < outstanding_queries; ++readback_count)
  {
    const u32 index = (m_query_readback_pos + readback_count) % PERF_QUERY_BUFFER_SIZE;
    ActiveQuery& entry = m_query_buffer[index];
    if (entry.fence_counter > completed_fence_counter)
      break;

    CounterReport report;
    std::memcpy(&report, &m_query_results[index], sizeof(report));

    u64 native_res_result = report.value * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() *
                            EFB_HEIGHT / g_framebuffer_manager->GetEFBHeight();
    if (g_ActiveConfig.iMultisamples > 1)
      native_res_result /= g_ActiveConfig.iMultisamples;
    m_results[entry.query_group].fetch_add(static_cast<u32>(native_res_result),
                                           std::memory_order_relaxed);

    entry = {};
  }

  m_query_readback_pos = (m_query_readback_pos + readback_count) % PERF_QUERY_BUFFER_SIZE;
  m_query_count.fetch_sub(readback_count, std::memory_order_relaxed);
}

void DKPerfQuery::PartialFlush(bool blocking)
{
  if (blocking || m_query_buffer[m_query_readback_pos].fence_counter ==
                      g_dk_command_buffer_mgr->GetCurrentFenceCounter())
  {
    DKGfx::GetInstance()->ExecuteCommandBuffer(blocking);
  }

  ReadbackQueries();
}
}  // namespace Deko3D
