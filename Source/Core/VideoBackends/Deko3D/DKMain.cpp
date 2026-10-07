// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/VideoBackend.h"

#include "Common/MsgHandler.h"

#include "VideoBackends/Deko3D/DKBoundingBox.h"
#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKObjectCache.h"
#include "VideoBackends/Deko3D/DKPerfQuery.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"
#include "VideoBackends/Deko3D/DKSwapChain.h"
#include "VideoBackends/Deko3D/DKVertexManager.h"

#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  // Report Vulkan so videocommon's shader generators emit the mesa/nouveau GLSL dialect that uam
  // consumes, and so clip space/depth range/framebuffer origin all match how we configure the
  // device (DepthZeroToOne | OriginUpperLeft).
  g_backend_info.api_type = APIType::Vulkan;
  g_backend_info.MaxTextureSize = 16384;
  g_backend_info.bUsesLowerLeftOrigin = false;
  g_backend_info.bSupportsExclusiveFullscreen = false;
  g_backend_info.bSupportsDualSourceBlend = true;
  g_backend_info.bSupportsPrimitiveRestart = true;
  g_backend_info.bSupportsGeometryShaders = true;
  g_backend_info.bSupportsComputeShaders = true;
  g_backend_info.bSupports3DVision = false;
  g_backend_info.bSupportsEarlyZ = true;
  g_backend_info.bSupportsBindingLayout = true;
  g_backend_info.bSupportsGSInstancing = true;
  g_backend_info.bSupportsPostProcessing = true;
  g_backend_info.bSupportsClipControl = true;
  g_backend_info.bSupportsSSAA = true;
  g_backend_info.bSupportsBitfield = true;
  g_backend_info.bSupportsDynamicSamplerIndexing = true;
  g_backend_info.bSupportsFragmentStoresAndAtomics = true;
  g_backend_info.bSupportsCopyToVram = true;
  // Enabling Deko3D's rasterizer depth clamp causes unstable clipping and widespread flickering.
  // This didn't take 12 hours straight to find. Why would you ask that?
  g_backend_info.bSupportsDepthClamp = false;
  g_backend_info.bSupportsReversedDepthRange = true;
  g_backend_info.bSupportsLogicOp = true;
  g_backend_info.bSupportsMultithreading = false;
  g_backend_info.bSupportsLodBiasInSampler = true;
  g_backend_info.bSupportsSettingObjectNames = false;
  g_backend_info.bSupportsPartialMultisampleResolve = true;
  g_backend_info.bSupportsDynamicVertexLoader = true;
  g_backend_info.bSupportsVSLinePointExpand = true;
  g_backend_info.bSupportsLargePoints = true;
  g_backend_info.bSupportsCoarseDerivatives = true;
  g_backend_info.bSupportsTextureQueryLevels = true;

  g_backend_info.bSupportsShaderBinaries = true;
  g_backend_info.bSupportsBackgroundCompiling = true;

  g_backend_info.bSupportsBBox = true;

  g_backend_info.bSupportsPaletteConversion = true;

  g_backend_info.bSupportsST3CTextures = true;
  g_backend_info.bSupportsBPTCTextures = true;

  // TODO: GPU texture decoding shows wrong textures in multiple games.
  g_backend_info.bSupportsGPUTextureDecoding = false;
  g_backend_info.bSupportsDepthReadback = true;
  g_backend_info.bSupportsPartialDepthCopies = true;

  g_backend_info.bSupportsPipelineCacheData = false;

  g_backend_info.Adapters.clear();
  g_backend_info.AAModes = {1, 2, 4};
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  g_dk_context = DKContext::Create();
  if (!g_dk_context)
  {
    PanicAlertFmt("Failed to create deko3d device.");
    return false;
  }

  g_dk_command_buffer_mgr = std::make_unique<DKCommandBufferManager>();
  if (!g_dk_command_buffer_mgr->Initialize())
  {
    PanicAlertFmt("Failed to create deko3d command buffers.");
    Shutdown();
    return false;
  }

  g_dk_object_cache = DKObjectCache::Create();
  if (!g_dk_object_cache)
  {
    PanicAlertFmt("Failed to create the deko3d object cache.");
    Shutdown();
    return false;
  }

  // The vertex manager binds through the state tracker as soon as it is initialized, so it has to
  // exist before InitializeShared.
  if (!DKStateTracker::CreateInstance())
  {
    PanicAlertFmt("Failed to create the deko3d state tracker.");
    Shutdown();
    return false;
  }

  UpdateActiveConfig();

  std::unique_ptr<DKSwapChain> swap_chain;
  if (wsi.render_surface)
  {
    swap_chain = DKSwapChain::Create(wsi.render_surface);
    if (!swap_chain)
    {
      PanicAlertFmt("Failed to create deko3d swapchain.");
      Shutdown();
      return false;
    }
  }

  auto gfx = std::make_unique<DKGfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<DKVertexManager>();
  auto perf_query = std::make_unique<DKPerfQuery>();
  auto bounding_box = std::make_unique<DKBoundingBox>();

  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
  if (g_dk_context)
    g_dk_context->WaitIdle();

  ShutdownShared();

  // The state tracker's dummy textures defer their memory back to the command buffer manager, so
  // it has to go first.
  DKStateTracker::DestroyInstance();
  g_dk_object_cache.reset();
  g_dk_command_buffer_mgr.reset();
  g_dk_context.reset();
}
}  // namespace Deko3D
