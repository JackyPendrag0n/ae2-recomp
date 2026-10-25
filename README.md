<div align="center">

# 🐒 Ape Escape 2 — native PC port

**A static recompilation of the PS2 classic for Linux, Windows and macOS.**
Co-op split screen · play as monkeys, Natalie, the Professor or Specter · up to 8× resolution · 60 Hz · widescreen

[![build check](https://github.com/JackyPendrag0n/ae2-recomp/actions/workflows/build.yml/badge.svg)](https://github.com/JackyPendrag0n/ae2-recomp/actions/workflows/build.yml)
![Linux](https://img.shields.io/badge/Linux-works-brightgreen?logo=linux&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-works-brightgreen?logo=windows&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-untested-yellow?logo=apple&logoColor=white)

</div>

> [!IMPORTANT]
> ## 💿 You must bring your own ISO
>
> **This repository contains no game.** No ISO, no BIOS, no game files, no code generated from the game, and no ready-made builds. You need **your own copy of Ape Escape 2 (PAL, SCES-50885)**, ripped **from your own disc** to an `.iso`. The setup script turns it into the files the build needs, on your machine only.
>
> **Do not ask for ISOs, game files, `port/gen` or compiled builds in the issues, and do not post them.** Such requests and links are deleted.

Every function of the PS2 game's executable is translated to C ahead of time, and a small runtime stands in for the PS2 hardware. It is not an emulator running the original MIPS code, so it runs natively at full speed.

## ✨ Features

| | |
|---|---|
| 🎮 **2-player co-op** | Split screen, Hikaru and Kakeru together on two controllers (or keyboard + controller). Pick *2 Players: On* on the title menu. Bosses go after whichever player is nearer, and switches, items and objects react to both players. |
| 🐵 **New playable characters** | From the debug menu (**F1 → Play as**): any of the seven kinds of monkey, each with its own ability (bombs, machine gun, homing missiles, Magic Punch, sprint, shield), **Natalie**, **the Professor**, or **Specter** with his own moves (energy shots, warp dash). |
| 🖥️ **Modern graphics** | OpenGL renderer at 1–8× the PS2 resolution (4× ≈ 4K), 16:9 widescreen, 50/60 Hz, field of view, brightness, all from a new *Graphics* page in the game's own Options menu. An accurate software renderer is the fallback. |
| ⚡ **Fast** | Multithreaded: the game, the VU1 vector unit and the GS renderer each run on their own thread. Hand-written C for the busiest VU microprograms. |
| 🛠️ **Debug menu** | Level select, unlock all stages and gadgets, free camera, no clip, FPS counter. |
| 🎬 **Everything else** | FMVs with sound, memory card saves to a folder, keyboard or any SDL gamepad. |

The full feature list, controls and options are in [docs/PORT.md](docs/PORT.md).

This project is not affiliated with or endorsed by Sony Interactive Entertainment. "Ape Escape" is a trademark of its owner.

---

## What you need

| | |
|---|---|
| Your game | **Ape Escape 2, PAL, SCES-50885**, as an `.iso` ripped from your own disc (a raw `.bin` also works). Other releases (NTSC-U, NTSC-J) are not supported. No PS2 BIOS is needed. |
| Python | Python 3.8 or newer with `pyelftools` (`python3 -m pip install pyelftools`) |
| A C compiler and SDL2 | per platform, see [Building](#3-build) |
| A PC | 64-bit; a 4-core CPU from the last ~8 years for full speed. OpenGL 3.3 for the GPU renderer (a software renderer is the fallback) |

## 1. Get the source

```sh
git clone https://github.com/JackyPendrag0n/ae2-recomp.git
cd ae2-recomp
```

## 2. Set up from your ISO

```sh
python3 -m pip install pyelftools
python3 tools/setup_game.py /path/to/ApeEscape2.iso
```

(On Windows use `python` instead of `python3`, and a path like `C:\Games\ApeEscape2.iso`.)

This does three things, all locally:

1. extracts your disc into `game/` (use `--game-dir` to put it elsewhere);
2. checks that it is the PAL release this port is written for (by checksum of `SCES_508.85`);
3. generates the C sources in `port/gen/` from the game's executable (a few seconds).

If you already have the disc extracted to a folder, pass the folder instead of the `.iso`.

**Never commit or upload `game/` or `port/gen/`.** They are made from the game; `.gitignore` already excludes them.

### Ripping your disc

Any tool that makes a plain 2048-byte-sector `.iso` of a DVD works, for example:

- **Windows:** ImgBurn → *Create image file from disc*.
- **Linux:** `dd if=/dev/sr0 of=ApeEscape2.iso bs=2048 status=progress`
- **macOS:** `diskutil list` to find the drive, `diskutil unmountDisk /dev/diskN`, then `dd if=/dev/rdiskN of=ApeEscape2.iso bs=2048`.

## 3. Build

Each platform has its own script in `platform/`. They all leave the program in `dist/<platform>/`.

### Linux

```sh
# Debian/Ubuntu
sudo apt install build-essential libsdl2-dev python3-pip
# Fedora:  sudo dnf install gcc make SDL2-devel python3-pip
# Arch:    sudo pacman -S base-devel sdl2 python-pip

./platform/linux/build.sh
```

A full build takes a few minutes (most of it is the ~9,000 generated functions). x86-64 and ARM64 both build.

### Windows

**Option A: natively, with MSYS2** (recommended)

1. Install [MSYS2](https://www.msys2.org) and open the **MSYS2 MINGW64** shell from the Start menu.
2. Install the tools:
   ```sh
   pacman -S --needed git make mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python mingw-w64-x86_64-python-pyelftools
   ```
   (If the pyelftools package is not found: `pacman -S mingw-w64-x86_64-python-pip` and `python -m pip install pyelftools`.)
3. In that shell, clone the repo, run the setup (`python tools/setup_game.py /c/Games/ApeEscape2.iso --game-dir dist/windows/game`), then:
   ```sh
   ./platform/windows/build-msys2.sh
   ```

**Option B: cross-compile on Linux**

```sh
sudo apt install mingw-w64 make curl
./platform/windows/build-cross.sh
```

This downloads the official SDL2 MinGW package once and builds a self-contained `ae2.exe` (SDL2 is linked in, no DLLs to carry around), and puts it with `run-ae2.bat` in `dist/windows/`. Copy that folder to the Windows PC.

### macOS

```sh
xcode-select --install          # compiler
brew install sdl2 python
python3 -m pip install pyelftools
./platform/macos/build.sh
```

On Apple Silicon this builds a native ARM64 program (the port's SSE maths goes through [sse2neon](https://github.com/DLTcollab/sse2neon)); on an Intel Mac it builds x86-64. `ARCH=x86_64` or `ARCH=arm64` picks one explicitly. See the top of `platform/macos/build.sh` for using the official SDL2.framework instead of Homebrew.

## 4. Play

```sh
dist/linux/ae2 game                 # Linux
dist/macos/ae2 game                 # macOS
dist\windows\ae2.exe game           # Windows (or double-click run-ae2.bat)
```

The argument is the folder with your disc files. If you leave it out, the program looks for a folder named `game` **next to the program**, so on Windows you can put your files in `dist\windows\game` and just double-click `ae2.exe` or `run-ae2.bat`.

Saves go to `memcard/` and settings to `ae2-settings.ini` in the folder you start it from.

**Controls (keyboard):** left stick W A S D · right stick I J K L · ✕ Space · ○ E · □ Q · △ F · L1/L2 1/2 · R1/R2 Shift or 3/4 · START Enter · SELECT Backspace · F1 debug menu · F11 fullscreen · Shift+Esc quit. Any SDL-compatible gamepad works; the game needs both analog sticks.

**2 players:** pick *2 Players: On* on the title menu. Player 2 uses the second controller (or the first controller when player 1 is on the keyboard). Details in [docs/PORT.md](docs/PORT.md#two-players-experimental).

## Platform status

| Platform | Status |
|---|---|
| Linux x86-64 | Main development platform. The repository build produces the same frames as the reference build. |
| Windows x86-64 | Works. Builds with MinGW-w64 (natively with MSYS2, or cross-compiled from Linux). Plays on real Windows PCs, and the frames match the Linux build within floating-point rounding. |
| macOS | The build files are in place but have not been tested on a Mac yet. The ARM64 code path was checked by running the Linux ARM64 build on an emulated ARM CPU. Reports welcome. |

**Why are there no downloads?** A compiled build contains code translated from the game's executable, so it can't be shared any more than the ISO can. Everyone builds their own from their own disc; it takes a few minutes. The *build check* badge above only compiles the runtime on each platform (Linux x86-64 and ARM64, Windows, macOS ARM64 and Intel), without any game code, and publishes nothing.

## Troubleshooting

- **"does not match the PAL release"** — your disc is a different version. Only SCES-50885 (PAL) is supported.
- **"port/gen is missing"** — run `tools/setup_game.py` before building.
- **Slow motion** — the game logic is tied to the frame rate, so a slow frame plays in slow motion. Use the GPU renderer, a lower resolution in *Options > Graphics*, and keep frame skipping on (the default). 2-player split screen needs the most CPU.
- **No GPU renderer** — it needs OpenGL 3.3. The software renderer is used automatically otherwise.
- **macOS says the program is damaged / from an unidentified developer** — it is an unsigned local build: `xattr -dr com.apple.quarantine dist/macos`.
- **Windows: the window closes immediately** — start it from `run-ae2.bat`, which keeps the log window open on errors.

## Repository layout

```
recomp/        EE and IOP recompilers (Python), function overrides, VU microprogram recipe
tools/         setup_game.py, iso_extract.py, vu_programs.py, vu_recomp.py
port/src/      the runtime: kernel, GS renderers, sound, IPU (FMV), input, 2-player mode
port/include/  CPU semantics (128-bit registers, MMI, FPU, VU0 macro ops)
port/compat/   POSIX gaps on Windows, SSE-on-ARM for Apple Silicon
platform/      build scripts for Linux, Windows and macOS
docs/PORT.md   features, options, environment variables, how it is put together
```

## Rules for contributions and forks

- Never add game files, ISOs, BIOS files, `port/gen/`, captured VU programs, or compiled binaries of the port to the repository or to GitHub releases. A compiled `ae2` contains code translated from the game, so it is as much the game's as `port/gen/` is.
- New VU microprograms go into `recomp/vu_programs.recipe` as ELF offsets (`tools/vu_programs.py make`), never as code.

## Credits

- Menu lettering rendered from Fira Sans Black (SIL Open Font Licence); debug font from DejaVu Sans Mono (Bitstream Vera licence).
- [sse2neon](https://github.com/DLTcollab/sse2neon) (MIT) in `port/compat/arm64/`.
- [SDL2](https://www.libsdl.org) (zlib licence).
