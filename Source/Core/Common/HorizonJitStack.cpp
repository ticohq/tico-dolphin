// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/HorizonJitStack.h"

#ifdef __SWITCH__

#include <atomic>
#include <cstddef>

#include <switch.h>

#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/ScopeGuard.h"

namespace Common::HorizonJitStack
{
namespace
{
constexpr size_t HORIZON_PAGE_SIZE = 0x1000;

constexpr size_t PROBE_SIZE = HORIZON_PAGE_SIZE * 3;

constexpr size_t OVERPOP_SLACK = HORIZON_PAGE_SIZE;

std::atomic<uintptr_t> s_probe_page{0};
std::atomic<bool> s_probe_faulted{false};

std::atomic<uintptr_t> s_armed_guard{0};
std::atomic<size_t> s_armed_guard_size{0};

std::atomic<uintptr_t> s_active_stack{0};
std::atomic<size_t> s_active_stack_size{0};

bool TakeGuardFault(void* page)
{
  NOTICE_LOG_FMT(COMMON,
                 "JIT stack guard: faulting {} on purpose. Hanging or dying here means Horizon "
                 "raises no recoverable fault for a stripped page.",
                 fmt::ptr(page));

  s_probe_faulted.store(false, std::memory_order_relaxed);
  s_probe_page.store(reinterpret_cast<uintptr_t>(page), std::memory_order_release);
  Common::ScopeGuard page_guard([] { s_probe_page.store(0, std::memory_order_release); });

  asm volatile("str wzr, [%0]" : : "r"(page) : "memory");

  return s_probe_faulted.load(std::memory_order_acquire);
}

bool ProbeHeap()
{
  void* const block = Common::AllocateAlignedMemory(PROBE_SIZE, HORIZON_PAGE_SIZE);
  if (!block)
    return false;
  Common::ScopeGuard block_guard([block] { Common::FreeAlignedMemory(block); });

  void* const page = static_cast<u8*>(block) + HORIZON_PAGE_SIZE;

  const Result result = svcSetMemoryPermission(page, HORIZON_PAGE_SIZE, Perm_None);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(COMMON, "JIT stack guard: svcSetMemoryPermission on heap failed: {:#010x}",
                 result);
    return false;
  }

  const bool faulted = TakeGuardFault(page);
  svcSetMemoryPermission(page, HORIZON_PAGE_SIZE, Perm_Rw);
  return faulted;
}

bool ProbeCodeMemory()
{
  void* const source = Common::AllocateAlignedMemory(PROBE_SIZE, HORIZON_PAGE_SIZE);
  if (!source)
    return false;
  Common::ScopeGuard source_guard([source] { Common::FreeAlignedMemory(source); });

  virtmemLock();
  void* const mapped = virtmemFindCodeMemory(PROBE_SIZE, HORIZON_PAGE_SIZE);
  VirtmemReservation* const reservation =
      mapped ? virtmemAddReservation(mapped, PROBE_SIZE) : nullptr;
  virtmemUnlock();

  if (!reservation)
  {
    WARN_LOG_FMT(COMMON, "JIT stack guard: no code memory address space for {} bytes.", PROBE_SIZE);
    return false;
  }
  Common::ScopeGuard reservation_guard([reservation] {
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
  });

  const Handle self = envGetOwnProcessHandle();
  const u64 mapped_addr = reinterpret_cast<u64>(mapped);

  Result result =
      svcMapProcessCodeMemory(self, mapped_addr, reinterpret_cast<u64>(source), PROBE_SIZE);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(COMMON, "JIT stack guard: svcMapProcessCodeMemory failed: {:#010x}", result);
    return false;
  }
  Common::ScopeGuard mapping_guard([self, mapped_addr, source] {
    svcUnmapProcessCodeMemory(self, mapped_addr, reinterpret_cast<u64>(source), PROBE_SIZE);
  });

  result = svcSetProcessMemoryPermission(self, mapped_addr, PROBE_SIZE, Perm_Rw);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(COMMON, "JIT stack guard: code memory would not take Perm_Rw: {:#010x}", result);
    return false;
  }

  const u64 guard_addr = mapped_addr + HORIZON_PAGE_SIZE;
  result = svcSetProcessMemoryPermission(self, guard_addr, HORIZON_PAGE_SIZE, Perm_None);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(COMMON,
                 "JIT stack guard: svcSetProcessMemoryPermission on one page of a code memory "
                 "mapping failed: {:#010x}",
                 result);
    return false;
  }

  const bool faulted = TakeGuardFault(reinterpret_cast<void*>(guard_addr));
  svcSetProcessMemoryPermission(self, guard_addr, HORIZON_PAGE_SIZE, Perm_Rw);
  return faulted;
}

GuardSource Probe()
{
  NOTICE_LOG_FMT(COMMON, "JIT stack guard: probing for memory a guard page can be armed on.");

  if (ProbeHeap())
  {
    NOTICE_LOG_FMT(COMMON, "JIT stack guard: heap pages work.");
    return GuardSource::Heap;
  }
  if (ProbeCodeMemory())
  {
    NOTICE_LOG_FMT(COMMON, "JIT stack guard: code memory pages work.");
    return GuardSource::CodeMemory;
  }

  WARN_LOG_FMT(COMMON, "JIT stack guard: nothing here can be guarded, so the BLR optimization "
                       "cannot be enabled on this firmware.");
  return GuardSource::None;
}
}  // namespace

GuardSource GetGuardSource()
{
  static const GuardSource source = Probe();
  return source;
}

Stack Allocate(size_t size, size_t guard_offset, size_t guard_size)
{
  // TODO: give the code memory path an allocator of its own if a console ever probes that way.
  if (GetGuardSource() != GuardSource::Heap)
    return {};

  if (size % HORIZON_PAGE_SIZE != 0 || guard_offset % HORIZON_PAGE_SIZE != 0 ||
      guard_size % HORIZON_PAGE_SIZE != 0 || guard_offset + guard_size > size)
  {
    return {};
  }

  void* const base = Common::AllocateAlignedMemory(size + OVERPOP_SLACK, HORIZON_PAGE_SIZE);
  if (!base)
  {
    WARN_LOG_FMT(COMMON, "JIT stack guard: could not reserve a {} byte stack.", size);
    return {};
  }

  Stack stack;
  stack.base = static_cast<u8*>(base);
  stack.size = size;
  stack.guard = stack.base + guard_offset;
  stack.guard_size = guard_size;

  s_active_stack_size.store(size, std::memory_order_relaxed);
  s_active_stack.store(reinterpret_cast<uintptr_t>(stack.base), std::memory_order_release);

  NOTICE_LOG_FMT(COMMON, "JIT stack: {} bytes at {}, guard {} bytes at {}.", size,
                 fmt::ptr(stack.base), guard_size, fmt::ptr(stack.guard));
  return stack;
}

void Release(Stack& stack)
{
  if (!stack)
    return;

  Disarm(stack);
  if (s_active_stack.load(std::memory_order_relaxed) == reinterpret_cast<uintptr_t>(stack.base))
    s_active_stack.store(0, std::memory_order_release);
  Common::FreeAlignedMemory(stack.base);
  stack = {};
}

bool Arm(const Stack& stack)
{
  if (!stack)
    return false;

  const Result result = svcSetMemoryPermission(stack.guard, stack.guard_size, Perm_None);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(COMMON, "JIT stack guard: arming {} failed: {:#010x}", fmt::ptr(stack.guard),
                 result);
    return false;
  }

  s_armed_guard_size.store(stack.guard_size, std::memory_order_relaxed);
  s_armed_guard.store(reinterpret_cast<uintptr_t>(stack.guard), std::memory_order_release);
  return true;
}

void Disarm(const Stack& stack)
{
  if (!stack)
    return;

  s_armed_guard.store(0, std::memory_order_release);
  svcSetMemoryPermission(stack.guard, stack.guard_size, Perm_Rw);
}

bool IsGuardAddress(uintptr_t address)
{
  const uintptr_t guard = s_armed_guard.load(std::memory_order_acquire);
  return guard != 0 && address >= guard &&
         address < guard + s_armed_guard_size.load(std::memory_order_relaxed);
}

bool GetActiveStackRange(uintptr_t* start, uintptr_t* end)
{
  const uintptr_t base = s_active_stack.load(std::memory_order_acquire);
  if (base == 0)
    return false;
  *start = base;
  *end = base + s_active_stack_size.load(std::memory_order_relaxed);
  return true;
}

bool HandleProbeFault(uintptr_t fault_address, u64& pc)
{
  const uintptr_t page = s_probe_page.load(std::memory_order_acquire);
  if (page == 0 || fault_address < page || fault_address >= page + HORIZON_PAGE_SIZE)
    return false;

  s_probe_faulted.store(true, std::memory_order_release);
  pc += sizeof(u32);
  return true;
}
}  // namespace Common::HorizonJitStack

#endif
