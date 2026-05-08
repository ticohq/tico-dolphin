// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__SWITCH__) && !defined(__LIBRETRO__)

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace DolphinNX::BootTrace
{
inline void Log(const char* fmt, ...)
{
  constexpr bool kBootTraceEnabled = false;
  if (!kBootTraceEnabled)
    return;

  static std::mutex s_mutex;
  std::lock_guard<std::mutex> guard(s_mutex);

  FILE* fp = std::fopen("sdmc:/dolphin-nx.log", "a");
  if (!fp)
    return;

  va_list args;
  va_start(args, fmt);
  std::vfprintf(fp, fmt, args);
  va_end(args);
  std::fflush(fp);
  std::fclose(fp);
}
}  // namespace DolphinNX::BootTrace

#endif
