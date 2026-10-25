# Ape Escape 2 (SCES-50885) static recompilation
#
# port/gen must exist first: run  python3 tools/setup_game.py <your.iso>  from the repo root.
#
#   make                        native build: Linux, macOS, or Windows in an MSYS2 MINGW64 shell
#   make PLATFORM=windows CROSS=x86_64-w64-mingw32- SDL2_PREFIX=/path/to/SDL2/x86_64-w64-mingw32
#                               cross-compile ae2.exe on Linux (platform/windows/build-cross.sh does this)
#   make ARCH=arm64 / ARCH=x86_64   macOS: pick the CPU (default: this Mac's)
#   make CHECKS=1               debugging build with FPU NaN/Inf checks (ae2-check)

ifeq ($(OS),Windows_NT)
PLATFORM ?= windows
else ifeq ($(shell uname -s),Darwin)
PLATFORM ?= macos
else
PLATFORM ?= linux
endif

OPT     ?= -O1
VUOPT   ?= -O2
RTOPT   ?= -O2
# -ffp-contract=off: never fuse a*b+c into one FMA (ARM compilers do by default); the PS2 rounds each step
CFLAGS  += $(OPT) -g0 -DHAVE_SDL -fno-strict-aliasing -fwrapv -ffp-contract=off -Iinclude -Igen -Isrc -w -pthread
LDFLAGS += -pthread -lm
EXE     :=

# ------------------------------------------------------------------ Linux
ifeq ($(PLATFORM),linux)
SDL2_CONFIG ?= sdl2-config
CFLAGS  += $(shell $(SDL2_CONFIG) --cflags)
LDFLAGS += -rdynamic $(shell $(SDL2_CONFIG) --libs)
endif

# ------------------------------------------------------------------ macOS
ifeq ($(PLATFORM),macos)
CC      := clang
ARCH    ?= $(shell uname -m)
CFLAGS  += -arch $(ARCH) -Icompat/noelf
LDFLAGS += -arch $(ARCH)
ifeq ($(ARCH),arm64)
ARM64   := 1
endif
ifneq ($(SDL2_FRAMEWORK),)
# SDL2.framework from the official SDL release (universal: arm64 + x86_64)
CFLAGS  += -F$(SDL2_FRAMEWORK) -I$(SDL2_FRAMEWORK)/SDL2.framework/Headers
LDFLAGS += -F$(SDL2_FRAMEWORK) -framework SDL2 -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/../Frameworks
else
# Homebrew's sdl2-config points into include/SDL2; the port also includes <SDL2/SDL.h>
CFLAGS  += $(shell sdl2-config --cflags) -I$(shell sdl2-config --prefix)/include
LDFLAGS += $(shell sdl2-config --libs)
endif
endif

# ------------------------------------------------------------------ ARM64 (Apple Silicon, ARM Linux)
ifneq ($(PLATFORM),macos)
ifneq ($(findstring aarch64,$(shell $(CC) -dumpmachine)),)
ARM64   := 1
endif
endif
ifeq ($(ARM64),1)
CFLAGS  += -Icompat/arm64 -include compat/arm64/compat.h
endif

# ------------------------------------------------------------------ Windows (MinGW-w64)
ifeq ($(PLATFORM),windows)
CROSS   ?=
CC      := $(CROSS)gcc
EXE     := .exe
CFLAGS  += -Icompat/win -Icompat/noelf -include compat/win/compat.h -D__USE_MINGW_ANSI_STDIO=1 -Wa,-mbig-obj
ifneq ($(SDL2_PREFIX),)
CFLAGS  += -I$(SDL2_PREFIX)/include -I$(SDL2_PREFIX)/include/SDL2 -Dmain=SDL_main
SDL_LIBS := -L$(SDL2_PREFIX)/lib -lmingw32 -lSDL2main -lSDL2
else
CFLAGS  += $(shell sdl2-config --cflags)
SDL_LIBS := $(filter-out -mwindows,$(shell sdl2-config --libs))
endif
# a console window shows the log. Default: one self-contained ae2.exe (SDL2, libgcc and
# winpthreads linked in, only Windows system DLLs needed). STATIC_SDL=0: use SDL2.dll instead.
STATIC_SDL ?= 1
ifeq ($(STATIC_SDL),1)
LDFLAGS += -static $(SDL_LIBS) -lm -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid -mconsole -s
else
LDFLAGS += $(SDL_LIBS) -lwinmm -mconsole -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
endif
endif

GEN_SRCS := $(wildcard gen/code_*.c) gen/functable.c gen/vu1_progs.c gen/iop_sndn.c
RT_SRCS  := $(filter-out $(EXCLUDE),$(wildcard src/*.c))

# make CHECKS=1: a separate debugging binary (ae2-check, built in build-check/) that
# reports FPU operands which are NaN/Inf on the host (see AE2_FPCHECK in ps2.h)
ifeq ($(CHECKS),1)
B := build-check-$(PLATFORM)
TARGET := ae2-check$(EXE)
CFLAGS += -DAE2_FPCHECK
else
B := build-$(PLATFORM)
TARGET := ae2$(EXE)
endif
OBJS     := $(GEN_SRCS:%.c=$(B)/%.o) $(RT_SRCS:%.c=$(B)/%.o)

all: check-gen $(TARGET)

check-gen:
	@test -f gen/functable.c || { echo "port/gen is missing: run  python3 tools/setup_game.py <your.iso>  from the repository root first"; exit 1; }

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

$(B)/gen/iop_sndn.o: gen/iop_sndn.c src/iop_mod.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -c $< -o $@

$(B)/gen/vu1_progs.o: gen/vu1_progs.c src/vu_impl.h include/ps2.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(VUOPT) -c $< -o $@

$(B)/%.o: %.c include/ps2.h include/mmi.h include/vu0.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/src/gs.o: src/gs.c $(wildcard src/*.h) include/ps2.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O3 -Wall -Wno-unused -c $< -o $@

$(B)/src/%.o: src/%.c $(wildcard src/*.h) include/ps2.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(RTOPT) -Wall -Wno-unused -c $< -o $@

# The runtime alone, without port/gen: what the GitHub build check compiles
runtime: $(RT_SRCS:%.c=$(B)/%.o)

clean:
	rm -rf build-* ae2 ae2.exe ae2-check ae2-check.exe
.PHONY: all clean check-gen runtime
