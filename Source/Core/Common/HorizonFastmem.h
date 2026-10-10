// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __SWITCH__

#include <cstddef>

namespace Common::HorizonFastmem
{
// Both 4 GiB guest views, back to back (no guards on Switch).
constexpr std::size_t ARENA_SIZE = 2 * 0x1'0000'0000ull;

// Reserve the arena's address space while it is still in one piece; the
// game's own reservation takes it later. Call first thing in main().
void ReserveArenaAddressSpace();

bool IsArenaSupported();
bool AreReadOnlyMappingsSupported();
}  // namespace Common::HorizonFastmem

#endif
