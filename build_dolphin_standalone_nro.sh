#!/bin/bash
# Build Dolphin as a standalone NRO for Nintendo Switch
set -e

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITPPC=$DEVKITPRO/devkitPPC
export DEVKITA64=$DEVKITPRO/devkitA64

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_nx_standalone"
ROMFS_DIR="${BUILD_DIR}/romfs"
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"
TICO_NRO_VERSION="${TICO_NRO_VERSION:-0.0.8}"
TICO_NX_DIR="${TICO_NX_DIR:-${SCRIPT_DIR}/../../../tico-nx}"

echo "=== Dolphin NX Standalone Build (no libretro) ==="
echo "Source: ${SCRIPT_DIR}"
echo "Build:  ${BUILD_DIR}"
echo "NVK:    ${MESA_NVK_DIR}"
echo "Version: ${TICO_NRO_VERSION}"
echo "Tico NX assets: ${TICO_NX_DIR} (optional)"
echo ""

if [ ! -f "${DEVKITPRO}/portlibs/switch/lib/libSDL2.a" ]; then
  echo "Installing SDL2 portlib for Switch..."
  dkp-pacman -Sy --noconfirm switch-sdl2 2>/dev/null || true
fi

if [ "$1" = "clean" ]; then
  echo "Cleaning build directory..."
  rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

cmake -B "${BUILD_DIR}" "${SCRIPT_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/Source/Core/DolphinNX/nx-toolchain.cmake" \
  -DSWITCH_STANDALONE=ON \
  -DSWITCH=OFF \
  -DLIBRETRO=OFF \
  -DLIBRETRO_STATIC=OFF \
  -DENABLE_QT=OFF \
  -DENABLE_NOGUI=OFF \
  -DENABLE_CLI_TOOL=OFF \
  -DENABLE_SDL=OFF \
  -DENABLE_VULKAN=ON \
  -DENABLE_LLVM=OFF \
  -DENABLE_LTO=ON \
  -DENABLE_AUTOUPDATE=OFF \
  -DENABLE_ANALYTICS=OFF \
  -DUSE_DISCORD_PRESENCE=OFF \
  -DUSE_MGBA=OFF \
  -DUSE_SFML=OFF \
  -DMESA_NVK_DIR="${MESA_NVK_DIR}" \
  -DTICO_NRO_VERSION="${TICO_NRO_VERSION}" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}" --target dolphin-nx -j"$(nproc)"

echo ""
echo "=== Packaging NRO ==="

rm -rf "${ROMFS_DIR}/fonts" "${ROMFS_DIR}/lang" "${ROMFS_DIR}/Sys" "${ROMFS_DIR}/config"
mkdir -p "${ROMFS_DIR}"
cp -R "${SCRIPT_DIR}/Source/Core/DolphinNX/Assets/fonts" "${ROMFS_DIR}/"
cp -R "${SCRIPT_DIR}/Source/Core/DolphinNX/Assets/lang" "${ROMFS_DIR}/"
# Dolphin's Sys tree, seeded to sdmc:/tico/system/gc/Sys on first run (see main.cpp).
cp -R "${SCRIPT_DIR}/Data/Sys" "${ROMFS_DIR}/"

# Profile hotfix payload for 0.0.8. Dolphin normally reads profiles from
# sdmc:/tico/config/cores/profiles/dolphin; these two files are force-reseeded
# once by Source/Core/DolphinNX/main.cpp so existing 0.0.7 installs receive the
# horizontal Wii Remote Joy-Con mapping fix.
BUNDLED_DOLPHIN_PROFILE_SRC="${SCRIPT_DIR}/Source/Core/DolphinNX/Assets/config/cores/profiles/dolphin"
TICO_NX_DOLPHIN_PROFILE_SRC="${TICO_NX_DIR}/assets/config/cores/profiles/dolphin"
DOLPHIN_PROFILE_SRC="${TICO_DOLPHIN_PROFILE_SRC:-}"
if [ -z "${DOLPHIN_PROFILE_SRC}" ]; then
  if [ -f "${TICO_NX_DOLPHIN_PROFILE_SRC}/handheld.json" ] && \
     [ -f "${TICO_NX_DOLPHIN_PROFILE_SRC}/joycon_dual.json" ]; then
    DOLPHIN_PROFILE_SRC="${TICO_NX_DOLPHIN_PROFILE_SRC}"
  else
    DOLPHIN_PROFILE_SRC="${BUNDLED_DOLPHIN_PROFILE_SRC}"
  fi
fi
DOLPHIN_PROFILE_DST="${ROMFS_DIR}/config/cores/profiles/dolphin"
if [ ! -f "${DOLPHIN_PROFILE_SRC}/handheld.json" ] || \
   [ ! -f "${DOLPHIN_PROFILE_SRC}/joycon_dual.json" ]; then
  echo "Missing Dolphin profile hotfix files in ${DOLPHIN_PROFILE_SRC}" >&2
  echo "Set TICO_DOLPHIN_PROFILE_SRC to a directory containing handheld.json and joycon_dual.json." >&2
  exit 1
fi
echo "Dolphin profile hotfix assets: ${DOLPHIN_PROFILE_SRC}"
mkdir -p "${DOLPHIN_PROFILE_DST}"
cp "${DOLPHIN_PROFILE_SRC}/handheld.json" "${DOLPHIN_PROFILE_DST}/"
cp "${DOLPHIN_PROFILE_SRC}/joycon_dual.json" "${DOLPHIN_PROFILE_DST}/"

nacptool --create \
  "tico Dolphin" \
  "ticoverse.com, dolphin-emu" \
  "${TICO_NRO_VERSION}" \
  "${BUILD_DIR}/dolphin.nacp"

cp "${BUILD_DIR}/Binaries/dolphin-nx" "${BUILD_DIR}/Binaries/dolphin-nx.debug.elf"
${DEVKITA64}/bin/aarch64-none-elf-strip --strip-all "${BUILD_DIR}/Binaries/dolphin-nx"

elf2nro \
  "${BUILD_DIR}/Binaries/dolphin-nx" \
  "${BUILD_DIR}/dolphin.nro" \
  --nacp="${BUILD_DIR}/dolphin.nacp" \
  --romfsdir="${ROMFS_DIR}"

echo ""
echo "=== Done ==="
echo "Output: ${BUILD_DIR}/dolphin.nro"
echo ""
echo "Deploy to Switch:"
echo "  cp ${BUILD_DIR}/dolphin.nro /path/to/sd/switch/dolphin/"
echo ""
echo "Launch from hbmenu with a ROM file to chainload."
