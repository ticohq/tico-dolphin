// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "VideoCommon/Constants.h"

namespace Deko3D
{
// Number of command buffers in flight. Matches the Vulkan backend so frame pacing and the stream
// buffers' fence tracking behave the same way.
constexpr size_t NUM_COMMAND_BUFFERS = 8;

// Backing memory handed to each command buffer up front. The cbAddMem callback grows a command
// buffer past this on demand and the extra chunks are then reused every frame, so these sizes only
// need to cover the common case.
constexpr u32 INIT_COMMAND_BUFFER_SIZE = 64 * 1024;
constexpr u32 DRAW_COMMAND_BUFFER_SIZE = 256 * 1024;

// Size of each additional slice handed to a command buffer that runs out of space.
constexpr u32 COMMAND_BUFFER_GROWTH_SIZE = 256 * 1024;

// Shared upload ring for texture data. Larger uploads use a temporary block so a single texture
// cannot pin a large portion of the ring until its command buffer retires.
constexpr u32 TEXTURE_UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;
constexpr u32 STAGING_TEXTURE_UPLOAD_THRESHOLD = 4 * 1024 * 1024;

// Maximum number of vertex attributes, matching the Vulkan backend.
constexpr u32 MAX_VERTEX_ATTRIBUTES = 16;

// Shader local ("scratch") memory each warp slot gets.
constexpr u32 PER_WARP_SCRATCH_MEMORY_SIZE = 32 * 1024;

// Uniform buffers for GX pipelines. The same buffer is bound at the same index on every stage that
// reads it.
constexpr u32 UBO_BINDING_PS = 0;
constexpr u32 UBO_BINDING_VS = 1;
constexpr u32 UBO_BINDING_CUST = 2;
constexpr u32 UBO_BINDING_GS = 3;
constexpr u32 NUM_UBO_BINDINGS = 4;

// Utility pipelines have a single uniform buffer shared by all stages.
constexpr u32 UBO_BINDING_UTILITY = 0;

// Storage buffers. The spaces are per-stage.
constexpr u32 SSBO_BINDING_BBOX = 0;
constexpr u32 SSBO_BINDING_VERTEX = 1;

// Combined image and sampler bindings.
constexpr u32 NUM_PIXEL_SHADER_SAMPLERS = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS;
constexpr u32 NUM_COMPUTE_SHADER_SAMPLERS = VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS;

constexpr u32 TEXEL_BUFFER_BINDING_BASE = NUM_PIXEL_SHADER_SAMPLERS;
constexpr u32 NUM_TEXEL_BUFFERS = 2;
constexpr u32 NUM_FRAGMENT_TEXTURE_BINDINGS = TEXEL_BUFFER_BINDING_BASE + NUM_TEXEL_BUFFERS;

// Image descriptors churn per draw, so they are allocated from a fence-tracked ring rather than
// rewritten in place.
constexpr u32 NUM_IMAGE_DESCRIPTORS = 64 * 1024;

// Sampler descriptors are keyed by SamplerState and only written when a new state first appears, so
// a small fixed table owned by the object cache is enough.
constexpr u32 NUM_SAMPLER_DESCRIPTORS = 1024;
}  // namespace Deko3D
