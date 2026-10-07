// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKMemoryTracker.h"

#include <charconv>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

#include "Common/Logging/Log.h"

namespace Deko3D::MemoryTracker
{
namespace
{
struct Allocation
{
  u64 addr = 0;
  u64 size = 0;
  std::string label;
  u64 serial = 0;
};

std::mutex s_mutex;
std::map<u64, Allocation> s_live;
std::deque<Allocation> s_recently_freed;
u64 s_next_serial = 1;

constexpr size_t MAX_RECENTLY_FREED = 512;

// How far past an allocation an address can land and still be worth blaming on it.
constexpr u64 OVERRUN_SLACK = 1024 * 1024;

const Allocation* Lookup(const std::map<u64, Allocation>& allocations, u64 addr)
{
  auto iter = allocations.upper_bound(addr);
  if (iter == allocations.begin())
    return nullptr;

  const Allocation& candidate = std::prev(iter)->second;
  const u64 offset = addr - candidate.addr;
  return offset < candidate.size + OVERRUN_SLACK ? &candidate : nullptr;
}

void LogAllocation(const char* state, const Allocation& allocation, u64 addr)
{
  const u64 offset = addr - allocation.addr;
  const char* containment = offset < allocation.size ? "inside" : "past the end of";
  ERROR_LOG_FMT(VIDEO, "deko3d: 0x{:010x} is {} #{} '{}' (0x{:010x}+0x{:x}) at +0x{:x}, {}", addr,
                containment, allocation.serial, allocation.label, allocation.addr, allocation.size,
                offset, state);
}
}  // namespace

void Register(u64 addr, u64 size, std::string label)
{
  if (!addr || addr == DK_GPU_ADDR_INVALID)
    return;

  std::lock_guard guard(s_mutex);
  s_live[addr] = Allocation{addr, size, std::move(label), s_next_serial++};
}

void Unregister(u64 addr)
{
  if (!addr || addr == DK_GPU_ADDR_INVALID)
    return;

  std::lock_guard guard(s_mutex);
  const auto iter = s_live.find(addr);
  if (iter == s_live.end())
    return;

  s_recently_freed.push_back(std::move(iter->second));
  if (s_recently_freed.size() > MAX_RECENTLY_FREED)
    s_recently_freed.pop_front();
  s_live.erase(iter);
}

void RegisterMemBlock(DkMemBlock block, std::string label)
{
  if (block)
    Register(dkMemBlockGetGpuAddr(block), dkMemBlockGetSize(block), std::move(label));
}

void UnregisterMemBlock(DkMemBlock block)
{
  if (block)
    Unregister(dkMemBlockGetGpuAddr(block));
}

void ReportAddress(u64 addr)
{
  std::lock_guard guard(s_mutex);

  const Allocation* live = Lookup(s_live, addr);
  if (live)
    LogAllocation("still live", *live, addr);

  bool found_freed = false;
  for (auto iter = s_recently_freed.rbegin(); iter != s_recently_freed.rend(); ++iter)
  {
    if (addr < iter->addr || addr - iter->addr >= iter->size)
      continue;

    LogAllocation("freed", *iter, addr);
    found_freed = true;
  }

  if (!live && !found_freed)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: 0x{:010x} belongs to no tracked allocation ({} live, {} freed)",
                  addr, s_live.size(), s_recently_freed.size());
  }
}

void ReportFaultMessage(std::string_view message)
{
  constexpr std::string_view PREFIX = "Address: 0x";
  const size_t start = message.find(PREFIX);
  if (start == std::string_view::npos)
    return;

  std::string_view digits = message.substr(start + PREFIX.size());
  const size_t end = digits.find_first_not_of("0123456789abcdefABCDEF");
  if (end != std::string_view::npos)
    digits = digits.substr(0, end);

  u64 addr = 0;
  const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), addr, 16);
  if (result.ec != std::errc{})
    return;

  ReportAddress(addr);
}
}  // namespace Deko3D::MemoryTracker
