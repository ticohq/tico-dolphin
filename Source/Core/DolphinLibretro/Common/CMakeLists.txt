add_library(dolphin_libretro_common STATIC
  VFile.cpp
  Globals.cpp
  Options.cpp
)

target_include_directories(dolphin_libretro_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBRETRO_INCLUDE_DIR}
)

target_include_directories(videoogl PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBRETRO_INCLUDE_DIR}
)

# needed for VFS when overriding Source/Core/Common/IOFile.h
target_include_directories(common PUBLIC
  ${LIBRETRO_INCLUDE_DIR}
)

# needed for windows cmake compile
target_link_libraries(dolphin_libretro_common fmt)
