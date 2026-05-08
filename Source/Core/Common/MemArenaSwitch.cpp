// Copyright 2008 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemArena.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <switch.h>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace EMM
{
void SetLazyRegionInfo(uintptr_t base, size_t size);
}

namespace Common
{
namespace
{
constexpr unsigned SVC_MAP_PROCESS_MEMORY = 0x74;
constexpr unsigned SVC_UNMAP_PROCESS_MEMORY = 0x75;
constexpr unsigned SVC_MAP_PROCESS_CODE_MEMORY = 0x77;
constexpr unsigned SVC_UNMAP_PROCESS_CODE_MEMORY = 0x78;

void NxDumpAddressSpace(const char* when)
{
  u64 aslr_base = 0, aslr_size = 0, heap_base = 0, heap_size = 0, alias_base = 0, alias_size = 0,
      stack_base = 0, stack_size = 0;
  svcGetInfo(&aslr_base, 12, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&aslr_size, 13, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&heap_base, 4, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&heap_size, 5, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&alias_base, 2, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&alias_size, 3, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&stack_base, 14, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&stack_size, 15, CUR_PROCESS_HANDLE, 0);
}

bool HasSwitchFastmemSyscalls()
{
  return envIsSyscallHinted(SVC_MAP_PROCESS_MEMORY) &&
         envIsSyscallHinted(SVC_UNMAP_PROCESS_MEMORY) &&
         envIsSyscallHinted(SVC_MAP_PROCESS_CODE_MEMORY) &&
         envIsSyscallHinted(SVC_UNMAP_PROCESS_CODE_MEMORY);
}
}  // namespace

MemArena::MemArena() = default;

MemArena::~MemArena()
{
  ReleaseSHMSegment();
  ReleaseMemoryRegion();
}

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  NxDumpAddressSpace("GrabSHMSegment-entry");

  size_t aligned_size = (size + 0x1FFFFF) & ~size_t{0x1FFFFF};
  m_shm_buffer = memalign(0x200000, aligned_size);

  if (m_shm_buffer == nullptr)
  {
    ERROR_LOG_FMT(MEMMAP, "Switch: memalign failed to allocate {} bytes for SHM segment",
                  aligned_size);
    return;
  }

  memset(m_shm_buffer, 0, aligned_size);
  m_shm_size = aligned_size;

  if (!HasSwitchFastmemSyscalls())
  {
    WARN_LOG_FMT(MEMMAP,
                 "Switch: required fastmem syscalls are not hinted by the environment "
                 "(MapProcessMemory={}, UnmapProcessMemory={}, MapProcessCodeMemory={}, "
                 "UnmapProcessCodeMemory={}); falling back to non-fastmem views",
                 envIsSyscallHinted(SVC_MAP_PROCESS_MEMORY),
                 envIsSyscallHinted(SVC_UNMAP_PROCESS_MEMORY),
                 envIsSyscallHinted(SVC_MAP_PROCESS_CODE_MEMORY),
                 envIsSyscallHinted(SVC_UNMAP_PROCESS_CODE_MEMORY));
    INFO_LOG_FMT(MEMMAP, "Switch: Allocated {} bytes SHM backing buffer at {} (no fastmem)",
                 aligned_size, fmt::ptr(m_shm_buffer));
    return;
  }

  virtmemLock();

  m_rw_mirror = virtmemFindCodeMemory(aligned_size, 0x200000);
  if (m_rw_mirror == nullptr)
  {
    virtmemUnlock();
    ERROR_LOG_FMT(MEMMAP, "Switch: virtmemFindCodeMemory failed for {} bytes", aligned_size);
    INFO_LOG_FMT(MEMMAP, "Switch: Allocated {} bytes SHM backing buffer at {} (no fastmem)",
                 aligned_size, fmt::ptr(m_shm_buffer));
    return;
  }

  Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)m_rw_mirror,
                                      (u64)m_shm_buffer, aligned_size);
  if (R_FAILED(rc))
  {
    virtmemUnlock();
    m_rw_mirror = nullptr;
    ERROR_LOG_FMT(MEMMAP, "Switch: svcMapProcessCodeMemory(RW alias) failed: 0x{:X}", rc);
    INFO_LOG_FMT(MEMMAP, "Switch: Allocated {} bytes SHM backing buffer at {} (no fastmem)",
                 aligned_size, fmt::ptr(m_shm_buffer));
    return;
  }

  rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64)m_rw_mirror, aligned_size,
                                     Perm_Rw);
  if (R_FAILED(rc))
  {
    svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)m_rw_mirror, (u64)m_shm_buffer,
                              aligned_size);
    virtmemUnlock();
    m_rw_mirror = nullptr;
    ERROR_LOG_FMT(MEMMAP, "Switch: svcSetProcessMemoryPermission(RW alias) failed: 0x{:X}", rc);
    INFO_LOG_FMT(MEMMAP, "Switch: Allocated {} bytes SHM backing buffer at {} (no fastmem)",
                 aligned_size, fmt::ptr(m_shm_buffer));
    return;
  }

  m_code_mirror = m_rw_mirror;

  virtmemUnlock();

  INFO_LOG_FMT(
      MEMMAP,
      "Switch: Allocated {} bytes SHM backing buffer at {}, single code alias at {} (RW)",
      aligned_size, fmt::ptr(m_shm_buffer), fmt::ptr(m_rw_mirror));
}

void MemArena::ReleaseSHMSegment()
{
  if (m_rw_mirror)
  {
    virtmemLock();
   svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)m_rw_mirror, (u64)m_shm_buffer,
                              m_shm_size);
    virtmemUnlock();
    m_rw_mirror = nullptr;
    m_code_mirror = nullptr;
  }

  if (m_shm_buffer)
  {
    free(m_shm_buffer);
    m_shm_buffer = nullptr;
    m_shm_size = 0;
  }
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  if (!m_shm_buffer)
  {
    ERROR_LOG_FMT(MEMMAP, "CreateView called but no SHM buffer allocated");
    return nullptr;
  }

  if (static_cast<size_t>(offset) + size > m_shm_size)
  {
    ERROR_LOG_FMT(MEMMAP, "CreateView: offset {} + size {} exceeds buffer size {}", offset, size,
                  m_shm_size);
    return nullptr;
  }

  if (m_rw_mirror)
    return static_cast<u8*>(m_rw_mirror) + offset;

  return static_cast<u8*>(m_shm_buffer) + offset;
}

void MemArena::ReleaseView(void* view, size_t size)
{
  (void)view;
  (void)size;
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  NxDumpAddressSpace("ReserveMemoryRegion-entry");

  if (!m_code_mirror)
  {
    WARN_LOG_FMT(MEMMAP,
                 "Switch: ReserveMemoryRegion called but code mirror not available - fastmem "
                 "disabled");
    return nullptr;
  }

  size_t aligned_size = (memory_size + 0x1FFFFF) & ~size_t{0x1FFFFF};

  virtmemLock();
  m_reserved_region = virtmemFindAslr(aligned_size, 0x200000);
  if (m_reserved_region == nullptr)
  {
    virtmemUnlock();

    for (size_t probe : {size_t(0x1'0000'0000ull), size_t(0x4000'0000ull),
                         size_t(0x1000'0000ull), size_t(0x100'0000ull)})
    {
      virtmemLock();
      void* p = virtmemFindAslr(probe, 0x1000);
      virtmemUnlock();
    }
    ERROR_LOG_FMT(MEMMAP, "Switch: virtmemFindAslr failed for {} bytes", aligned_size);
    return nullptr;
  }

  m_reservation = virtmemAddReservation(m_reserved_region, aligned_size);
  if (m_reservation == nullptr)
  {
    virtmemUnlock();
    m_reserved_region = nullptr;
    ERROR_LOG_FMT(MEMMAP, "Switch: virtmemAddReservation failed");
    return nullptr;
  }

  m_reserved_region_size = aligned_size;
  virtmemUnlock();

  INFO_LOG_FMT(MEMMAP, "Switch: Reserved {} bytes fastmem region at {}", aligned_size,
               fmt::ptr(m_reserved_region));

  return static_cast<u8*>(m_reserved_region);
}

void MemArena::ReleaseMemoryRegion()
{
  if (m_reservation)
  {
    virtmemLock();
    virtmemRemoveReservation(static_cast<VirtmemReservation*>(m_reservation));
    virtmemUnlock();
    m_reservation = nullptr;
  }

  m_reserved_region = nullptr;
  m_reserved_region_size = 0;
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  if (!m_reserved_region || !m_code_mirror)
  {
    ERROR_LOG_FMT(MEMMAP, "Switch: MapInMemoryRegion called but fastmem not initialized");
    return nullptr;
  }

  if (!HasSwitchFastmemSyscalls())
  {
    ERROR_LOG_FMT(MEMMAP,
                  "Switch: MapInMemoryRegion called but required fastmem syscalls are not "
                  "hinted by the environment");
    return nullptr;
  }

  size_t aligned_size = (size + 0xFFF) & ~0xFFF;

  Result rc = svcMapProcessMemory(base, envGetOwnProcessHandle(),
                                  (u64)m_code_mirror + offset, aligned_size);
  if (R_FAILED(rc))
  {
    static bool s_logged_map_failure = false;
    if (!s_logged_map_failure)
    {
      s_logged_map_failure = true;
      WARN_LOG_FMT(MEMMAP,
                   "Switch: svcMapProcessMemory failed: 0x{:X} (base={}, offset={}, size={}) "
                   "— subsequent failures on this path will be silent (expected for "
                   "overlapping DBAT remaps)",
                   rc, fmt::ptr(base), offset, aligned_size);
    }
    return nullptr;
  }

  if (!ChangeMappingProtection(base, aligned_size, writeable))
  {
    svcUnmapProcessMemory(base, envGetOwnProcessHandle(), (u64)m_code_mirror + offset,
                          aligned_size);
    ERROR_LOG_FMT(MEMMAP,
                  "Switch: failed to set mapping protection (base={}, offset={}, size={}, "
                  "writeable={})",
                  fmt::ptr(base), offset, aligned_size, writeable);
    return nullptr;
  }

  return base;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  if (!view || size == 0)
    return true;

  if (writeable)
    return true;

  WARN_LOG_FMT(MEMMAP,
               "Switch: cannot make ProcessMemory mapping at {} (size {}) read-only; "
               "leaving as RW (inherited from source alias)",
               fmt::ptr(view), size);
  return true;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size, s64 shm_offset)
{
  if (!m_code_mirror || !view)
    return;

  size_t aligned_size = (size + 0xFFF) & ~0xFFF;
  if (shm_offset >= 0)
  {
    svcUnmapProcessMemory(view, envGetOwnProcessHandle(),
                          (u64)m_code_mirror + shm_offset, aligned_size);
  }
  else
  {
    ERROR_LOG_FMT(MEMMAP,
                  "Switch: UnmapFromMemoryRegion called without shm_offset for view {}",
                  fmt::ptr(view));
  }
}

size_t MemArena::GetPageSize() const
{
  return 0x1000;
}

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);

  if (size == 0)
    return nullptr;

  const size_t aligned_size = (size + 0xFFF) & ~0xFFF;

  u64 alias_base = 0, alias_size = 0;
  svcGetInfo(&alias_base, 2, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&alias_size, 3, CUR_PROCESS_HANDLE, 0);

  if (aligned_size > alias_size)
  {
    NOTICE_LOG_FMT(MEMMAP,
                   "Switch: LazyMemoryRegion size 0x{:x} exceeds alias region 0x{:x}; "
                   "LargeEntryMap will be disabled (using small fallback map)",
                   aligned_size, alias_size);
    return nullptr;
  }

  uintptr_t base = 0;
  uintptr_t probe = static_cast<uintptr_t>(alias_base);
  const uintptr_t alias_end = static_cast<uintptr_t>(alias_base + alias_size);
  while (probe < alias_end)
  {
    MemoryInfo info{};
    u32 page_info = 0;
    Result rc = svcQueryMemory(&info, &page_info, probe);
    if (R_FAILED(rc))
      break;

    const uintptr_t span_start = static_cast<uintptr_t>(info.addr);
    const uintptr_t span_end = span_start + static_cast<uintptr_t>(info.size);

    if (info.type == MemType_Unmapped)
    {
      const uintptr_t aligned_start = (span_start + 0xFFF) & ~uintptr_t{0xFFF};
      if (aligned_start < span_end && (span_end - aligned_start) >= aligned_size &&
          aligned_start >= alias_base && (aligned_start + aligned_size) <= alias_end)
      {
        base = aligned_start;
        break;
      }
    }

    if (span_end <= probe)
      break;
    probe = span_end;
  }

  if (base == 0)
  {
    NOTICE_LOG_FMT(MEMMAP,
                   "Switch: couldn't find a free {}-byte slot in the alias region for "
                   "LazyMemoryRegion; LargeEntryMap disabled",
                   aligned_size);
    return nullptr;
  }

  m_memory = reinterpret_cast<void*>(base);
  m_size = aligned_size;
  m_committed_pages.assign(aligned_size / SWITCH_PAGE_SIZE, 0);

  EMM::SetLazyRegionInfo(base, aligned_size);

  INFO_LOG_FMT(MEMMAP,
               "Switch: LazyMemoryRegion reserved {} bytes at {} inside the alias region "
               "(commit-on-fault)",
               aligned_size, fmt::ptr(m_memory));

  return m_memory;
}

void LazyMemoryRegion::MakeMemoryPageCommitted(size_t page_index)
{
  constexpr size_t kCommitWindow = 64 * 1024;
  constexpr size_t kPagesPerWindow = kCommitWindow / SWITCH_PAGE_SIZE;

  size_t window_first_page = page_index & ~(kPagesPerWindow - 1);
  size_t window_last_page = window_first_page + kPagesPerWindow;
  if (window_last_page > m_committed_pages.size())
    window_last_page = m_committed_pages.size();

  void* window_addr = static_cast<u8*>(m_memory) + window_first_page * SWITCH_PAGE_SIZE;
  size_t window_bytes = (window_last_page - window_first_page) * SWITCH_PAGE_SIZE;

  Result rc = svcMapPhysicalMemory(window_addr, window_bytes);
  if (R_SUCCEEDED(rc))
  {
    for (size_t i = window_first_page; i < window_last_page; ++i)
      m_committed_pages[i] = 1;
    return;
  }

  void* page_addr = static_cast<u8*>(m_memory) + page_index * SWITCH_PAGE_SIZE;
  rc = svcMapPhysicalMemory(page_addr, SWITCH_PAGE_SIZE);
  m_committed_pages[page_index] = 1;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);
  for (size_t i = 0; i < m_committed_pages.size(); ++i)
  {
    if (m_committed_pages[i])
    {
      std::memset(static_cast<u8*>(m_memory) + i * SWITCH_PAGE_SIZE, 0, SWITCH_PAGE_SIZE);
    }
  }
}

void LazyMemoryRegion::Release()
{
  if (!m_memory)
    return;

  EMM::SetLazyRegionInfo(0, 0);

  const uintptr_t lazy_base = reinterpret_cast<uintptr_t>(m_memory);
  const uintptr_t lazy_end = lazy_base + m_size;
  uintptr_t probe = lazy_base;
  while (probe < lazy_end)
  {
    MemoryInfo info{};
    u32 page_info = 0;
    Result rc = svcQueryMemory(&info, &page_info, probe);
    if (R_FAILED(rc))
      break;

    const uintptr_t span_start = static_cast<uintptr_t>(info.addr);
    const uintptr_t span_end = span_start + static_cast<uintptr_t>(info.size);

    if (info.type != MemType_Unmapped)
    {
      const uintptr_t unmap_start = std::max(span_start, lazy_base);
      const uintptr_t unmap_end = std::min(span_end, lazy_end);
      if (unmap_start < unmap_end)
        svcUnmapPhysicalMemory(reinterpret_cast<void*>(unmap_start),
                                    unmap_end - unmap_start);
    }

    if (span_end <= probe)
      break;  
  }

  m_committed_pages.clear();
  m_committed_pages.shrink_to_fit();
  m_memory = nullptr;
  m_size = 0;
}

}  // namespace Common