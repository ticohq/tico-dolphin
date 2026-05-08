// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef MMAN_H
#define MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define BreakReason _BreakReason
#include <switch.h>
#undef BreakReason

#define PROT_READ 0b001
#define PROT_WRITE 0b010
#define PROT_EXEC 0b100
#define PROT_NONE 0x0
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_SHARED 0x01
#define MAP_FIXED_NOREPLACE 0
#define MAP_NORESERVE 0
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

static inline int shm_open(const char *name, int oflag, mode_t mode) {
  return -1;
}
static inline int shm_unlink(const char *name) { return -1; }

static void *ptr_rw = NULL;

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd,
                         off_t offset) {
  (void)fd;
  (void)offset;
  (void)prot;
  (void)flags;

  size_t size = (len + 0xFFF) & ~0xFFF;
  virtmemLock();
  ptr_rw = virtmemFindCodeMemory(size, 0);
  virtmemUnlock();
  if (R_SUCCEEDED(svcMapProcessMemory(ptr_rw, envGetOwnProcessHandle(),
                                      (u64)addr, size))) {
    return ptr_rw;
  } else {
    printf("[NXJIT]: Jit failed!\n");
    return (void *)-1;
  }
}

static inline int mprotect(void *addr, size_t len, int prot) { return 0; }

static inline int munmap(void *addr, size_t len) {
  size_t size = (len + 0xFFF) & ~0xFFF;
  svcUnmapProcessMemory(ptr_rw, envGetOwnProcessHandle(), (u64)addr, size);
  printf("[NXJIT]: Jit closed\n");

  return 0;
}

#ifdef __cplusplus
};
#endif // MMAN_H
#endif
