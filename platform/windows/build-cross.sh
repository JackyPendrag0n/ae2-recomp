#!/usr/bin/env bash
# Windows build, cross-compiled on Linux with MinGW-w64:
#   sudo apt install mingw-w64 make curl     (Debian/Ubuntu)
#   ./platform/windows/build-cross.sh        ->  dist/windows/ae2.exe (self-contained, SDL2 built in)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
SDL_VER=2.30.12
CROSS=${CROSS:-x86_64-w64-mingw32-}
DEPS="$ROOT/platform/windows/.deps"
SDL="$DEPS/SDL2-$SDL_VER/x86_64-w64-mingw32"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found: sudo apt install mingw-w64"; exit 1; }
if [ ! -f port/gen/functable.c ]; then
  echo "port/gen is missing. Generate it from your own disc first:"
  echo "  python3 tools/setup_game.py /path/to/your/ApeEscape2.iso"
  exit 1
fi
if [ ! -f "$SDL/bin/SDL2.dll" ]; then
  echo "Downloading the official SDL $SDL_VER MinGW development package..."
  mkdir -p "$DEPS"
  curl -fL -o "$DEPS/sdl2.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
  tar -xzf "$DEPS/sdl2.tar.gz" -C "$DEPS"
fi

make -C port PLATFORM=windows CROSS="$CROSS" SDL2_PREFIX="$SDL" -j"$(nproc)"
mkdir -p dist/windows
cp port/ae2.exe platform/windows/run-ae2.bat dist/windows/
echo
echo "Built dist/windows/ae2.exe (no DLLs needed). Copy the dist/windows folder to the Windows PC,"
echo "put your extracted disc in its 'game' subfolder, and run ae2.exe."
