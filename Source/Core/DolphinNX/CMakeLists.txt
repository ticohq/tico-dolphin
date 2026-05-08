project(dolphin-nx)

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
  NetPlayStubs.cpp
  TicoCore.cpp
  Overlay/Overlay.cpp
  Overlay/TranslationManager.cpp
  Overlay/VulkanOverlay.cpp
)

target_compile_definitions(dolphin-nx PRIVATE
  __SWITCH__
  SWITCH
  HAVE_LIBNX
  AARCH64
  NX
  VK_USE_PLATFORM_VI_NN
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
if(DEFINED MESA_NVK_DIR)
  target_link_libraries(dolphin-nx PRIVATE
    ${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a
  )
endif()

# Nouveau DRM + compression (system libraries from devkitPro portlibs)
target_link_libraries(dolphin-nx PRIVATE
  -ldrm_nouveau
  -lzstd
  -lz
)

# libnx + SDL2 (from devkitPro portlibs, NOT bundled) — full paths to avoid search issues
target_link_libraries(dolphin-nx PRIVATE
  ${DEVKITPRO}/portlibs/switch/lib/libSDL2.a
  ${DEVKITPRO}/libnx/lib/libnx.a
  m
)

# Linker flags for NRO-compatible ELF
set_target_properties(dolphin-nx PROPERTIES
  OUTPUT_NAME dolphin-nx
  LINK_FLAGS "-Wl,--gc-sections -Wl,--allow-multiple-definition"
)

install(TARGETS dolphin-nx RUNTIME DESTINATION bin)
