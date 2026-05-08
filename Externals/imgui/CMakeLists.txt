if (NOT MSVC)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(SRCS
  imgui/imgui.cpp
  imgui/imgui_demo.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_widgets.cpp
)

# Vulkan backend is only needed by the DolphinNX standalone frontend.
if(SWITCH_STANDALONE)
  list(APPEND SRCS imgui/backends/imgui_impl_vulkan.cpp)
endif()

add_library(imgui STATIC ${SRCS})
dolphin_disable_warnings(imgui)

target_include_directories(imgui
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
)

target_compile_definitions(imgui
  PUBLIC
    IMGUI_USER_CONFIG="imgui_user_config.h"
)

if(SWITCH_STANDALONE)
  target_include_directories(imgui
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
      "${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include"
  )
  # Use Dolphin's dynamically-loaded Vulkan entry points rather than link-time
  # prototypes — call ImGui_ImplVulkan_LoadFunctions() before ImGui_ImplVulkan_Init().
  target_compile_definitions(imgui
    PUBLIC
      IMGUI_IMPL_VULKAN_NO_PROTOTYPES
  )
  target_compile_definitions(imgui
    PRIVATE
      IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
  )
endif()

target_link_libraries(imgui
  PRIVATE
    fmt::fmt
)
