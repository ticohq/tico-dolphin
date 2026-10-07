project(dolphin-nx)

set(TICO_NRO_VERSION "0.0.0" CACHE STRING "tico Dolphin NRO package version")

# Link directories MUST come before add_executable / target_link_libraries
link_directories(
  ${DEVKITPRO}/devkitA64/lib/
  ${DEVKITPRO}/libnx/lib/
  ${DEVKITPRO}/portlibs/switch/lib/
)

add_executable(dolphin-nx
  main.cpp
  Host.cpp
  Audio.cpp
  Input.cpp
  ControllerProfiles.cpp
  NetPlayStubs.cpp
  TicoCore.cpp
  Achievements.cpp
  Cheats.cpp
  Discs.cpp
  Saves.cpp
  Riivolution.cpp
  Portal.cpp
  Library.cpp
  LibraryScreen.cpp
  MbedTLSEntropy.cpp
  Overlay/OverlayRenderer.cpp
  Overlay/VulkanOverlay.cpp
  # tico's overlay and frontend pieces, shared with the other cores
  ${CMAKE_SOURCE_DIR}/tico/UsbStorage.cpp
  # the SD card read cache, from wine-nx
  ${CMAKE_SOURCE_DIR}/tico/sdcache/sd_cache.c
  ${CMAKE_SOURCE_DIR}/tico/sdcache/sd_cache_tico.c
  ${CMAKE_SOURCE_DIR}/tico/overlay/imgui_overlay.cpp
  ${CMAKE_SOURCE_DIR}/tico/overlay/overlay_ui.cpp
  ${CMAKE_SOURCE_DIR}/tico/overlay/ra_alerts.cpp
  ${CMAKE_SOURCE_DIR}/tico/overlay/tico_config.cpp
  ${CMAKE_SOURCE_DIR}/tico/overlay/translation_manager.cpp
)

target_compile_definitions(dolphin-nx PRIVATE
  __SWITCH__
  SWITCH
  HAVE_LIBNX
  AARCH64
  NX
  VK_USE_PLATFORM_VI_NN
  TICO_NRO_VERSION="${TICO_NRO_VERSION}"
)

# Optimization flags for Tegra X1 (Cortex-A57)
target_compile_options(dolphin-nx PRIVATE
  -O3
  -ffunction-sections
  -fdata-sections
  -ffast-math
  -ftree-vectorize
  -fomit-frame-pointer
)

# Include paths
target_include_directories(dolphin-nx PRIVATE
  "${CMAKE_SOURCE_DIR}/Source/Core"
  "${CMAKE_SOURCE_DIR}/tico"
  "${CMAKE_SOURCE_DIR}/tico/deps/usbhsfs/include"
  "${CMAKE_SOURCE_DIR}/Externals/picojson"
  "${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include"
  "${CMAKE_SOURCE_DIR}/Externals/VulkanMemoryAllocator/include"
  "${CMAKE_SOURCE_DIR}/Externals/imgui/imgui"
  "${CMAKE_SOURCE_DIR}/Externals/imgui/imgui/backends"
  "${CMAKE_SOURCE_DIR}/Externals/stb"
  "${DEVKITPRO}/libnx/include"
  "${DEVKITPRO}/portlibs/switch/include"
  "${DEVKITPRO}/portlibs/switch/include/SDL2"
)

# Mesa NVK include path (for vulkan headers if needed beyond bundled Externals)
if(DEFINED MESA_NVK_DIR)
  target_include_directories(dolphin-nx PRIVATE
    "${MESA_NVK_DIR}/../include"
  )

  target_link_directories(dolphin-nx PRIVATE
    "${MESA_NVK_DIR}/src/nouveau/vulkan"
    "${MESA_NVK_DIR}/src/vulkan/util"
    "${MESA_NVK_DIR}/src/nouveau/compiler"
    "${MESA_NVK_DIR}/src/compiler/spirv"
    "${MESA_NVK_DIR}/src/util"
    "${MESA_NVK_DIR}/src/nouveau/nil"
    "${MESA_NVK_DIR}/src/nouveau/mme"
    "${MESA_NVK_DIR}/src/nouveau/winsys"
    "${MESA_NVK_DIR}/src/nouveau/headers"
    "${MESA_NVK_DIR}/src/compiler/nir"
    "${MESA_NVK_DIR}/src/compiler"
    "${MESA_NVK_DIR}/src/compiler/rust"
    "${MESA_NVK_DIR}/src/util/blake3"
    "${MESA_NVK_DIR}/src/c11/impl"
  )
endif()

# Core Dolphin libraries. videovulkan is linked explicitly so the overlay
# can call VKGfx::SetOverlayCallback and reach g_vulkan_context directly.
target_link_libraries(dolphin-nx PRIVATE
  core
  uicommon
  videovulkan
  imgui
)

# NVK / Vulkan (static) — replaces OGL/EGL Mesa stack
# Use Mesa's public loaderless libvulkan.a package instead of linking
# libnvk.a and every support archive manually.
# A /nvk-build tree is used when there is one, otherwise switch-dev's Mesa
# for Horizon in the portlibs.
if(DEFINED MESA_NVK_DIR AND EXISTS "${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a")
  set(DOLPHIN_NX_VULKAN_LIBRARY "${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a")
else()
  set(DOLPHIN_NX_VULKAN_LIBRARY "${DEVKITPRO}/portlibs/switch/lib/libvulkan.a")
endif()
target_link_libraries(dolphin-nx PRIVATE ${DOLPHIN_NX_VULKAN_LIBRARY})

# The /nvk-build driver needs libdrm_nouveau; the portlibs one needs what its
# vulkan.pc names instead (libelf, expat). Both need zstd and zlib.
foreach(dep drm_nouveau elf expat zstd z)
  find_library(DOLPHIN_NX_VULKAN_DEP_${dep} ${dep}
    PATHS "${DEVKITPRO}/portlibs/switch/lib" NO_DEFAULT_PATH)
  if(DOLPHIN_NX_VULKAN_DEP_${dep})
    target_link_libraries(dolphin-nx PRIVATE ${DOLPHIN_NX_VULKAN_DEP_${dep}})
  endif()
endforeach()

# libusbhsfs (FAT/exFAT) that also reads NTFS, read-only, through usbntfs;
# linked by path so portlibs' FAT-only copy is not picked up instead.
target_link_libraries(dolphin-nx PRIVATE
  ${CMAKE_SOURCE_DIR}/tico/deps/usbhsfs/lib/libusbhsfs.a
  ${CMAKE_SOURCE_DIR}/tico/deps/usbhsfs/lib/libusbntfs.a
)

# libnx + SDL2 (from devkitPro portlibs, NOT bundled) — full paths to avoid search issues
target_link_libraries(dolphin-nx PRIVATE
  ${DEVKITPRO}/portlibs/switch/lib/libSDL2.a
)
# switch-dev's SDL2 is built with its EGL video driver.
foreach(dep EGL GLESv2 glapi)
  find_library(DOLPHIN_NX_SDL_DEP_${dep} ${dep}
    PATHS "${DEVKITPRO}/portlibs/switch/lib" NO_DEFAULT_PATH)
  if(DOLPHIN_NX_SDL_DEP_${dep})
    target_link_libraries(dolphin-nx PRIVATE ${DOLPHIN_NX_SDL_DEP_${dep}})
  endif()
endforeach()
target_link_libraries(dolphin-nx PRIVATE
  ${DEVKITPRO}/libnx/lib/libnx.a
  m
)

# Linker flags for NRO-compatible ELF
set_target_properties(dolphin-nx PROPERTIES
  OUTPUT_NAME dolphin-nx
  LINK_FLAGS "-Wl,--gc-sections -Wl,--allow-multiple-definition"
)

install(TARGETS dolphin-nx RUNTIME DESTINATION bin)

# reads only: Dolphin also changes the card through fsdev* directly, which a
# kept stat or held write would not see; at most 96 MB of chunks
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/tico/sdcache/sd_cache.c
  PROPERTIES COMPILE_DEFINITIONS "SD_CACHE_STATS_AND_HELD_WRITES=0;SD_CACHE_POOL_MAX=768")
