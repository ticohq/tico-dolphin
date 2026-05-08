# Nintendo Switch (NX) cross-compilation toolchain for devkitPro
# Based on YabaSanshiro NX toolchain

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Skip executable link test — bare-metal cross compiler needs -specs to link
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED DEVKITPRO)
  set(DEVKITPRO "/opt/devkitpro/")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

set(DEVKITPRO_BIN "${DEVKITPRO}/devkitA64/bin/")
set(PREFIX "aarch64-none-elf-")

# Toolchain binaries
set(CMAKE_C_COMPILER   ${DEVKITPRO_BIN}${PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${DEVKITPRO_BIN}${PREFIX}g++)
set(CMAKE_ASM_COMPILER ${DEVKITPRO_BIN}${PREFIX}as)
set(CMAKE_AR           ${DEVKITPRO_BIN}${PREFIX}gcc-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       ${DEVKITPRO_BIN}${PREFIX}gcc-ranlib)
set(CMAKE_C_COMPILER_AR ${DEVKITPRO_BIN}${PREFIX}gcc-ar CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB ${DEVKITPRO_BIN}${PREFIX}gcc-ranlib CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_AR ${DEVKITPRO_BIN}${PREFIX}gcc-ar CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB ${DEVKITPRO_BIN}${PREFIX}gcc-ranlib CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP        ${DEVKITPRO_BIN}${PREFIX}strip)
set(CMAKE_NM           ${DEVKITPRO_BIN}${PREFIX}nm)
set(CMAKE_OBJCOPY      ${DEVKITPRO_BIN}${PREFIX}objcopy)
set(CMAKE_LINKER       ${DEVKITPRO_BIN}${PREFIX}ld)

set(CMAKE_ASM_COMPILER_AR     ${DEVKITPRO_BIN}${PREFIX}gcc-ar CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER_RANLIB ${DEVKITPRO_BIN}${PREFIX}gcc-ranlib)

# Architecture flags
set(ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -O2 -ffunction-sections ${ARCH}")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} -fno-rtti")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")

# Linker specs for NRO output — _INIT ensures it's applied exactly once
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${DEVKITPRO}/libnx/switch.specs")

# System include paths
include_directories(SYSTEM
  "${DEVKITPRO}/devkitA64/include"
  "${DEVKITPRO}/libnx/include"
  "${DEVKITPRO}/portlibs/switch/include"
)

# Library search paths
link_directories(
  ${DEVKITPRO}/devkitA64/lib
  ${DEVKITPRO}/libnx/lib
  ${DEVKITPRO}/portlibs/switch/lib
)

add_definitions(-DNX -D__SWITCH__)
