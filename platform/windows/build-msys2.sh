#!/usr/bin/env bash
# Windows build, natively, in an MSYS2 "MINGW64" shell (https://www.msys2.org):
#   pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python mingw-w64-x86_64-python-pyelftools
#   ./platform/windows/build-msys2.sh        ->  dist/windows/ae2.exe (+ the DLLs it needs)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if [ "${MSYSTEM:-}" != "MINGW64" ] && [ "${MSYSTEM:-}" != "UCRT64" ]; then
  echo "Run this from the MSYS2 MINGW64 (or UCRT64) shell, not the plain MSYS shell."
  exit 1
fi
if [ ! -f port/gen/functable.c ]; then
  echo "port/gen is missing. Generate it from your own disc first:"
  echo "  python tools/setup_game.py /c/path/to/your/ApeEscape2.iso"
  exit 1
fi

make -C port PLATFORM=windows -j"$(nproc)"
mkdir -p dist/windows
cp port/ae2.exe dist/windows/
# SDL2 is linked in statically; copy any DLL that still comes from the MSYS2 prefix (normally none)
for dll in $(ldd port/ae2.exe | awk '/\/(mingw64|ucrt64)\// {print $3}'); do cp -u "$dll" dist/windows/; done || true
cp platform/windows/run-ae2.bat dist/windows/
echo
echo "Built dist/windows/ae2.exe"
echo "Put your extracted disc in dist/windows/game (python tools/setup_game.py does that with --game-dir dist/windows/game),"
echo "then double-click run-ae2.bat or ae2.exe."
