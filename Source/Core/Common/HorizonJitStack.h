// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __SWITCH__

#include <cstddef>
#include <cstdint>

#include "Common/CommonTypes.h"

namespace Common::HorizonJitStack
{
enum class GuardSource
{
  None,
  Heap,
  CodeMemory,
};

GuardSource GetGuardSource();

struct Stack
{
  u8* base = nullptr;
  size_t size = 0;
  u8* guard = nullptr;
  size_t guard_size = 0;

  u8* Top() const { return base + size; }

  explicit operator bool() const { return base != nullptr; }
};

Stack Allocate(size_t size, size_t guard_offset, size_t guard_size);
void Release(Stack& stack);

bool Arm(const Stack& stack);
void Disarm(const Stack& stack);

bool IsGuardAddress(uintptr_t address);

bool GetActiveStackRange(uintptr_t* start, uintptr_t* end);

bool HandleProbeFault(uintptr_t fault_address, u64& pc);
}  // namespace Common::HorizonJitStack

#endif
