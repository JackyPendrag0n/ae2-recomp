#!/usr/bin/env bash
# macOS build: ./platform/macos/build.sh   ->  dist/macos/ae2
# Needs: Xcode command line tools (xcode-select --install), Homebrew SDL2 (brew install sdl2),
# Python 3 with pyelftools, and port/gen (tools/setup_game.py).
#
#   ARCH=arm64  ./platform/macos/build.sh   Apple Silicon, native (the default on an M1/M2/M3/M4 Mac);
#                                           the SSE maths runs through sse2neon
#   ARCH=x86_64 ./platform/macos/build.sh   Intel Macs (or Rosetta 2 on Apple Silicon: needs an
#                                           x86_64 SDL2, e.g. SDL2_FRAMEWORK below)
#   SDL2_FRAMEWORK=/path/to/dir  use SDL2.framework from the official SDL release .dmg (universal)
#                                instead of Homebrew; the framework is copied next to the program
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
ARCH=${ARCH:-$(uname -m)}

command -v clang >/dev/null || { echo "clang not found: xcode-select --install"; exit 1; }
if [ -z "${SDL2_FRAMEWORK:-}" ] && ! command -v sdl2-config >/dev/null; then
  echo "SDL2 not found: brew install sdl2   (or set SDL2_FRAMEWORK, see the top of this script)"; exit 1
fi
if [ ! -f port/gen/functable.c ]; then
  echo "port/gen is missing. Generate it from your own disc first:"
  echo "  python3 tools/setup_game.py /path/to/your/ApeEscape2.iso"
  exit 1
fi

make -C port PLATFORM=macos ARCH="$ARCH" ${SDL2_FRAMEWORK:+SDL2_FRAMEWORK="$SDL2_FRAMEWORK"} -j"$(sysctl -n hw.ncpu)"
mkdir -p dist/macos
cp port/ae2 dist/macos/ae2
if [ -n "${SDL2_FRAMEWORK:-}" ]; then rm -rf dist/macos/SDL2.framework; cp -R "$SDL2_FRAMEWORK/SDL2.framework" dist/macos/; fi
# unsigned local build: clear the quarantine flag so Gatekeeper lets it run
xattr -dr com.apple.quarantine dist/macos 2>/dev/null || true
echo
echo "Built dist/macos/ae2 ($ARCH)"
echo "Run it with:  dist/macos/ae2 \"$ROOT/game\""
