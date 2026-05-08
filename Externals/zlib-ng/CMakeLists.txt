set(ZLIB_ENABLE_TESTS OFF)
set(ZLIB_COMPAT ON)
set(SKIP_INSTALL_ALL ON)

# newlib (devkitA64) lacks posix_memalign; the static-library try-compile falsely detects it
if(SWITCH_STANDALONE)
  set(HAVE_POSIX_MEMALIGN OFF CACHE BOOL "" FORCE)
endif()

option(BUILD_SHARED_LIBS "Build shared library" OFF)

add_subdirectory(zlib-ng)

# Set ZLIB variables for find_package used by other projects
set(ZLIB_INCLUDE_DIR ${CMAKE_BINARY_DIR}/zlib-ng CACHE STRING "Path to zlib include directory")
set(ZLIB_LIBRARY ZLIB::ZLIB CACHE STRING "Path to zlib library")

# Setup zlib alias project so FindZLIB doesn't recreate it
add_library(ZLIB::ZLIB ALIAS zlib)
dolphin_disable_warnings(zlib)
