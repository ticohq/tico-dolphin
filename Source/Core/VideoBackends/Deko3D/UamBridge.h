// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

// The only symbol the uam blob exports. uam vendors its own copy of mesa 19.0, whose globals
// collide with (and are ABI-incompatible with) the newer mesa NXVK is built from, so everything
// else in the library is localised at link time.
extern "C" {
// Mirrors uam's pipeline_stage enum.
enum UamStage
{
  UamStage_Vertex = 0,
  UamStage_TessControl = 1,
  UamStage_TessEval = 2,
  UamStage_Geometry = 3,
  UamStage_Fragment = 4,
  UamStage_Compute = 5,
};

// Compiles GLSL to a DKSH blob. Returns a malloc'd blob of *out_size bytes on success and null on failure.
void* UamCompileGlsl(const char* glsl, int stage, size_t* out_size, char** out_log);
}
