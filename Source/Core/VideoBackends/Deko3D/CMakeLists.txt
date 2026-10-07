# Copyright 2026 Dolphin Emulator Project
# Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(videodeko3d
  Constants.h
  DKBoundingBox.cpp
  DKBoundingBox.h
  DKCommandBufferManager.cpp
  DKCommandBufferManager.h
  DKContext.cpp
  DKContext.h
  DKGfx.cpp
  DKGfx.h
  DKMain.cpp
  DKMemoryTracker.cpp
  DKMemoryTracker.h
  DKObjectCache.cpp
  DKObjectCache.h
  DKPerfQuery.cpp
  DKPerfQuery.h
  DKPipeline.cpp
  DKPipeline.h
  DKShader.cpp
  DKShader.h
  DKShaderCompiler.cpp
  DKShaderCompiler.h
  DKStateTracker.cpp
  DKStateTracker.h
  DKStreamBuffer.cpp
  DKStreamBuffer.h
  DKSwapChain.cpp
  DKSwapChain.h
  DKTexture.cpp
  DKTexture.h
  DKVertexFormat.cpp
  DKVertexFormat.h
  DKVertexManager.cpp
  DKVertexManager.h
  UamBridge.h
  UamStderr.h
  VideoBackend.h
)

if(ENABLE_FRAME_GENERATION)
  target_sources(videodeko3d PRIVATE
    DKFrameGenerationShaders.cpp
    DKFrameGenerationShaders.h
    DKFrameGenerator.cpp
    DKFrameGenerator.h
  )
  target_link_libraries(videodeko3d PRIVATE lsfg)
endif()

# UamBridge.cpp is deliberately absent above as it is compiled into the isolated uam blob instead, so
# that it can see uam's headers without the rest of the backend seeing uam's mesa (See CMake/Uam.cmake).
target_link_libraries(videodeko3d
PUBLIC
  common
  videocommon

PRIVATE
  deko3d
  uam
)
