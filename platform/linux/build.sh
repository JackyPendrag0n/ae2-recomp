#!/usr/bin/env bash
# Linux build: ./platform/linux/build.sh   ->  dist/linux/ae2
# Needs: gcc (or clang), make, SDL2 development files, and port/gen (tools/setup_game.py).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

missing=()
command -v make >/dev/null || missing+=(make)
command -v "${CC:-gcc}" >/dev/null || missing+=(gcc)
command -v sdl2-config >/dev/null || missing+=("SDL2 development package")
if ((${#missing[@]})); then
  echo "Missing: ${missing[*]}"
  echo "  Debian/Ubuntu: sudo apt install build-essential libsdl2-dev python3 python3-pip"
  echo "  Fedora:        sudo dnf install gcc make SDL2-devel python3 python3-pip"
  echo "  Arch:          sudo pacman -S base-devel sdl2 python python-pip"
  exit 1
fi
if [ ! -f port/gen/functable.c ]; then
  echo "port/gen is missing. Generate it from your own disc first:"
  echo "  python3 tools/setup_game.py /path/to/your/ApeEscape2.iso"
  exit 1
fi

make -C port PLATFORM=linux -j"$(nproc)"
mkdir -p dist/linux
cp port/ae2 dist/linux/ae2
echo
echo "Built dist/linux/ae2"
echo "Run it with:  dist/linux/ae2 \"$ROOT/game\""
echo "(or put a folder named 'game' with your disc files next to the program and run it with no arguments)"
