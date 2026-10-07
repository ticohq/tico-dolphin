// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKObjectCache.h"

#include <algorithm>
#include <array>

#include "Common/Align.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKStreamBuffer.h"

namespace Deko3D
{
std::unique_ptr<DKObjectCache> g_dk_object_cache;

DKObjectCache::~DKObjectCache() = default;

std::unique_ptr<DKObjectCache> DKObjectCache::Create()
{
  std::unique_ptr<DKObjectCache> cache(new DKObjectCache());
  if (!cache->Initialize())
    return nullptr;

  return cache;
}

bool DKObjectCache::Initialize()
{
  const u32 size = static_cast<u32>(Common::AlignUp(
      sizeof(DkSamplerDescriptor) * NUM_SAMPLER_DESCRIPTORS, DK_MEMBLOCK_ALIGNMENT));

  m_descriptor_block = dk::MemBlockMaker{g_dk_context->GetDevice(), size}
                           .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_ZeroFillInit)
                           .create();
  if (!m_descriptor_block)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate {} bytes of sampler descriptors", size);
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_descriptor_block, "sampler descriptors");

  m_descriptors = static_cast<DkSamplerDescriptor*>(m_descriptor_block.getCpuAddr());
  m_descriptor_set_addr = m_descriptor_block.getGpuAddr();

  m_texture_upload_buffer = DKStreamBuffer::Create(TEXTURE_UPLOAD_BUFFER_SIZE, "texture upload");
  if (!m_texture_upload_buffer)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate the texture upload ring");
    return false;
  }

  m_pipeline_capture_memblock =
      dk::MemBlockMaker{g_dk_context->GetDevice(), DK_MEMBLOCK_ALIGNMENT}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  m_pipeline_capture_cmdbuf = dk::CmdBufMaker{g_dk_context->GetDevice()}.create();
  if (!m_pipeline_capture_memblock || !m_pipeline_capture_cmdbuf)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate pipeline capture resources");
    return false;
  }
  m_pipeline_capture_cmdbuf.addMemory(m_pipeline_capture_memblock, 0, DK_MEMBLOCK_ALIGNMENT);

  // Slot 0 is the point sampler, used for every binding that has no state of its own yet.
  dk::Sampler point_sampler;
  point_sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest);
  point_sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
  m_point_sampler_index = AllocateSampler(point_sampler);
  return true;
}

u32 DKObjectCache::AllocateSampler(const DkSampler& sampler)
{
  const u32 index = m_next_index++;
  dkSamplerDescriptorInitialize(&m_descriptors[index], &sampler);
  return index;
}

u32 DKObjectCache::GetSamplerIndex(const SamplerState& state)
{
  const auto iter = m_sampler_cache.find(state);
  if (iter != m_sampler_cache.end())
    return iter->second;

  if (m_next_index >= NUM_SAMPLER_DESCRIPTORS)
  {
    WARN_LOG_FMT(VIDEO, "deko3d: sampler descriptor set is full, falling back to point sampling");
    return m_point_sampler_index;
  }

  static constexpr std::array<DkFilter, 2> filters = {DkFilter_Nearest, DkFilter_Linear};
  static constexpr std::array<DkMipFilter, 2> mip_filters = {DkMipFilter_Nearest,
                                                             DkMipFilter_Linear};
  static constexpr std::array<DkWrapMode, 4> wrap_modes = {
      DkWrapMode_ClampToEdge, DkWrapMode_Repeat, DkWrapMode_MirroredRepeat, DkWrapMode_ClampToEdge};

  dk::Sampler sampler;
  sampler.setFilter(filters[static_cast<u32>(state.tm0.min_filter.Value())],
                    filters[static_cast<u32>(state.tm0.mag_filter.Value())],
                    mip_filters[static_cast<u32>(state.tm0.mipmap_filter.Value())]);
  sampler.setWrapMode(wrap_modes[static_cast<u32>(state.tm0.wrap_u.Value())],
                      wrap_modes[static_cast<u32>(state.tm0.wrap_v.Value())],
                      DkWrapMode_ClampToEdge);
  sampler.setLodClamp(state.tm1.min_lod / 16.0f, state.tm1.max_lod / 16.0f);
  sampler.setLodBias(state.tm0.lod_bias / 256.0f);

  if (state.tm0.anisotropic_filtering != 0)
  {
    sampler.setMaxAnisotropy(
        static_cast<float>(1 << static_cast<u32>(state.tm0.anisotropic_filtering)));
  }

  const u32 index = AllocateSampler(sampler);
  m_sampler_cache.emplace(state, index);
  return index;
}

void DKObjectCache::ClearSamplerCache()
{
  m_sampler_cache.clear();
  m_next_index = m_point_sampler_index + 1;
}
}  // namespace Deko3D
