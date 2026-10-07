// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/PerfQueryBase.h"

namespace Deko3D
{
class DKPerfQuery final : public PerfQueryBase
{
public:
  DKPerfQuery();
  ~DKPerfQuery() override;

  bool Initialize() override;
  void EnableQuery(PerfQueryGroup group) override;
  void DisableQuery(PerfQueryGroup group) override;
  void ResetQuery() override;
  u32 GetQueryResult(PerfQueryType type) override;
  void FlushResults() override;
  bool IsFlushed() const override;

private:
  static constexpr u32 PERF_QUERY_BUFFER_SIZE = 512;

  struct CounterReport
  {
    u64 value;
    u64 timestamp;
  };
  static_assert(sizeof(CounterReport) == 16);

  struct ActiveQuery
  {
    u64 fence_counter = 0;
    PerfQueryGroup query_group = PQG_ZCOMP;
    bool has_value = false;
  };

  void ReadbackQueries();
  void PartialFlush(bool blocking);

  dk::UniqueMemBlock m_query_memblock;
  CounterReport* m_query_results = nullptr;
  std::array<ActiveQuery, PERF_QUERY_BUFFER_SIZE> m_query_buffer = {};
  u32 m_query_readback_pos = 0;
  u32 m_query_next_pos = 0;
};
}  // namespace Deko3D
