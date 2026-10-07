// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

// mbedtls's entropy source on Switch (MBEDTLS_ENTROPY_HARDWARE_ALT): there is no /dev/urandom,
// so the console's random service seeds every TLS connection (curl, the Wii's SSL).

#include <cstddef>

#include <switch.h>

extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
  (void)data;
  randomGet(output, len);
  *olen = len;
  return 0;
}
