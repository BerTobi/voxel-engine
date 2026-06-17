# Voxel Engine - GNU Makefile
# =============================
# One portable GL 2.1 / GLSL 1.20 engine core plus a thin platform layer with
# two interchangeable backends. We never compile both backends into one binary:
# the Linux/GLX build uses platform_linux.c, the Win32/WGL build uses
# platform_win32.c. Zero third-party libraries (no SDL/GLFW/GLEW); GL function
# pointers are loaded manually in gl_loader.c.
#
# Targets:
#   linux  (default)  native dev build  -> build/voxel       (link -lGL -lX11 -lm)
#   win               cross build       -> build/voxel.exe   (MinGW-w64 i686)
#   test              pure-C unit test  -> build/m1_test, runs it (no GL, no libs)
#   clean             remove build/
#
# Toolchain packages (Debian/Ubuntu names):
#   linux : build-essential  libgl1-mesa-dev  libx11-dev   (gcc + GL/X11 headers)
#   win   : gcc-mingw-w64-i686                              (i686-w64-mingw32-gcc)
#   test  : build-essential                                (gcc only)

# ---- Tools -------------------------------------------------------------------
CC      := gcc
WINCC   := i686-w64-mingw32-gcc

# ---- Version (single source of truth: src/version.h) -------------------------
# Parsed from the header so `make archive` files binaries under the right folder
# without a second place to bump. Factorio-style MAJOR.MINOR.PATCH, plus an
# optional PRERELEASE suffix ("-dev" while a MINOR is in progress, "" on release)
# so a work-in-progress build archives under e.g. 0.2.0-dev and never collides
# with the final 0.2.0.
VER_MAJOR := $(shell sed -n 's/^\#define VOXEL_VERSION_MAJOR *\([0-9]*\).*/\1/p' src/version.h)
VER_MINOR := $(shell sed -n 's/^\#define VOXEL_VERSION_MINOR *\([0-9]*\).*/\1/p' src/version.h)
VER_PATCH := $(shell sed -n 's/^\#define VOXEL_VERSION_PATCH *\([0-9]*\).*/\1/p' src/version.h)
VER_PRE   := $(shell sed -n 's/^\#define VOXEL_VERSION_PRERELEASE *"\([^"]*\)".*/\1/p' src/version.h)
VERSION   := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)$(VER_PRE)

# ---- Flags -------------------------------------------------------------------
# Common flags for every build: C99, all warnings, headers from src/.
# -fno-strict-aliasing is ledger-mandated: we punt a Voxel through u32<->struct
# views and type-pun mesh vertex bytes, so the compiler must not assume otherwise.
CFLAGS  := -std=c99 -Wall -Wextra -Isrc -fno-strict-aliasing

# Native dev build: optimize, but no -march (this host is x86-64, not the target).
LINUX_CFLAGS := $(CFLAGS) -O2

# Windows XP / Pentium M 780 (Dothan) build. The M170's CPU has SSE/SSE2 ONLY:
# without -march=pentium-m the cross-compiler may emit SSE3+/cmov that raise an
# illegal-instruction (#UD) on real M170 hardware. -mfpmath=sse keeps incidental
# float (matrices, fog) off the x87 stack; the CA hot loops are integer fixed
# point (Section 3.6), so this governs only render-side float. -ffunction/-fdata
# -sections + -Wl,--gc-sections strip dead code from the static binary.
WIN_CFLAGS := $(CFLAGS) -O2 -march=pentium-m -mtune=pentium-m -msse -msse2 \
	-mfpmath=sse -ffunction-sections -fdata-sections

# ---- Directories -------------------------------------------------------------
SRC     := src
BUILD   := build

# ---- Source sets -------------------------------------------------------------
# Portable engine core, shared by both platform builds (no backend .c here).
CORE    := \
	$(SRC)/material.c \
	$(SRC)/chunk.c \
	$(SRC)/mesher.c \
	$(SRC)/light.c \
	$(SRC)/sim.c \
	$(SRC)/worldgen.c \
	$(SRC)/world.c \
	$(SRC)/persist.c \
	$(SRC)/progress.c \
	$(SRC)/raycast.c \
	$(SRC)/player.c \
	$(SRC)/gl_loader.c \
	$(SRC)/render.c \
	$(SRC)/main.c

# Pure-C set: no GL, no platform, no external libraries. Compiles and runs
# anywhere a C99 compiler exists.
TEST_SRC := \
	$(SRC)/material.c \
	$(SRC)/chunk.c \
	$(SRC)/mesher.c \
	$(SRC)/test_mesher.c

# Per-backend platform layer (exactly one is added per build).
LINUX_PLATFORM := $(SRC)/platform_linux.c
WIN_PLATFORM   := $(SRC)/platform_win32.c

# 0.3 multiplayer: net.c is portable protocol; net_{linux,win32}.c are the
# per-backend socket syscalls (exactly one is added per build, like the platform
# layer). main.c calls net_* unconditionally, so both targets link net.c.
NET        := $(SRC)/net.c
NET_LINUX  := $(SRC)/net_linux.c
NET_WIN    := $(SRC)/net_win32.c

# Full source lists per target = engine core + the chosen backend + networking.
LINUX_SRC := $(CORE) $(LINUX_PLATFORM) $(NET) $(NET_LINUX)
WIN_SRC   := $(CORE) $(WIN_PLATFORM) $(NET) $(NET_WIN)

# ---- Link libraries ----------------------------------------------------------
# Linux/GLX: GL ICD, X11 windowing, libm for the float math (matrices/fog).
LINUX_LIBS := -lGL -lX11 -lm
# Win32/WGL: opengl32 ICD loader + gdi32 (pixel format / device context) +
# user32 (window/messages). -static + -static-libgcc so ZERO MinGW DLLs ship
# (no DLL hell on a bare XP install). --gc-sections drops unreferenced code.
# -mwindows makes a GUI subsystem exe (no console window).
WIN_LDFLAGS := -static -static-libgcc -Wl,--gc-sections -mwindows
# ws2_32: Winsock2 (0.3 multiplayer, net_win32.c).
WIN_LIBS    := -lopengl32 -lgdi32 -luser32 -lws2_32

# ---- Targets -----------------------------------------------------------------
.PHONY: all linux win test testsim testworld testpersist testprogress testraycast testedit testplayer testnet version archive clean

# Default target: native dev build.
all: linux

# version: print the version parsed from src/version.h (the one source of truth).
version:
	@echo $(VERSION)

# archive: build BOTH targets and preserve their executables under
# archive/<version>/ - a permanent, per-version backup (by request). `cp -n`
# (no-clobber) means an already-archived version's binaries are NEVER overwritten:
# bump src/version.h before re-archiving, or the existing ones are kept. Run this
# on every release, alongside a git tag v<version>.
archive: linux win
	mkdir -p archive/$(VERSION)
	cp -n $(BUILD)/voxel     archive/$(VERSION)/voxel
	cp -n $(BUILD)/voxel.exe archive/$(VERSION)/voxel.exe
	@echo "archived $(VERSION) -> archive/$(VERSION)/ (voxel + voxel.exe)"

# linux: native development build (GL 2.1 via Mesa compatibility profile + X11).
# apt: build-essential libgl1-mesa-dev libx11-dev
linux: | $(BUILD)
	$(CC) $(LINUX_CFLAGS) $(LINUX_SRC) -o $(BUILD)/voxel $(LINUX_LIBS)

# win: cross build for 32-bit Windows XP SP3 (the real target hardware).
# apt: gcc-mingw-w64-i686
win: | $(BUILD)
	$(WINCC) $(WIN_CFLAGS) $(WIN_SRC) -o $(BUILD)/voxel.exe $(WIN_LDFLAGS) $(WIN_LIBS)

# test: build and run the pure-C mesher unit test. No GL, no platform, no libs.
# apt: build-essential
test: | $(BUILD)
	$(CC) $(CFLAGS) $(TEST_SRC) -o $(BUILD)/m1_test
	$(BUILD)/m1_test

# testsim: build and run the pure-C heat-sim unit test (M3). No GL, no platform.
# -lm because sim.c/material.c pull in the float-side codec/thermal helpers.
# progress.c is linked because sim.c's M9 emit hook references prog_emit /
# ProgressEvent; with a NULL sink the sim stays byte-identical, so these 19
# tests still pass (the field defaults NULL).
# apt: build-essential
testsim: | $(BUILD)
	$(CC) -std=c99 -Wall -Wextra -Isrc -o $(BUILD)/m3_test $(SRC)/material.c $(SRC)/chunk.c $(SRC)/sim.c $(SRC)/progress.c $(SRC)/test_sim.c -lm
	$(BUILD)/m3_test

# testworld: build and run the pure-C chunk-streaming unit test (M7). Exercises
# the WorldStore hash/slab-pool, deterministic worldgen, loaded-window policy,
# eviction, and neighbour wiring. No GL, no platform. -lm because worldgen.c /
# material.c pull in the float-side helpers.
# apt: build-essential
testworld: | $(BUILD)
	$(CC) -std=c99 -Wall -Wextra -Isrc -o $(BUILD)/m7_test $(SRC)/material.c $(SRC)/chunk.c $(SRC)/worldgen.c $(SRC)/world.c $(SRC)/persist.c $(SRC)/test_world.c -lm
	$(BUILD)/m7_test

# testpersist: build and run the pure-C world-persistence unit test (M8).
# Exercises palette+RLE record codec, region-file header/index, save/load round
# trip, and gen-vs-stored (only CHUNK_MODIFIED chunks persist), all against a
# temp save dir. No GL, no platform, no world.c. -lm because worldgen.c /
# material.c pull in the float-side helpers.
# apt: build-essential
testpersist: | $(BUILD)
	$(CC) -std=c99 -Wall -Wextra -Isrc -o $(BUILD)/m8_test $(SRC)/material.c $(SRC)/chunk.c $(SRC)/worldgen.c $(SRC)/persist.c $(SRC)/test_persist.c -lm
	$(BUILD)/m8_test

# testprogress: build and run the pure-C progression-observer unit test (M9).
# The progression layer is a READ-ONLY observer: the sim emits ProgressEvents on
# emergent transitions (melt/freeze/temp-tier), the observer drains them into
# discoveries + an empirical journal + emergent capability tiers. Links sim.c so
# the test can exercise the real emit hook, and asserts the read-only invariant
# (NULL sink => byte-identical chunk voxels at every tick). No GL, no platform.
# -lm because sim.c/material.c/progress.c pull in the float-side codec helpers.
# apt: build-essential
testprogress: | $(BUILD)
	$(CC) -std=c99 -Wall -Wextra -Isrc -o $(BUILD)/m9_test $(SRC)/material.c $(SRC)/chunk.c $(SRC)/sim.c $(SRC)/progress.c $(SRC)/test_progress.c -lm
	$(BUILD)/m9_test

# testraycast: the pure-math voxel DDA (raycast.c) against a synthetic grid. No
# GL, no platform, no world. The 0.2 block break/place targeting core.
# apt: build-essential
testraycast: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/ray_test $(SRC)/raycast.c $(SRC)/test_raycast.c -lm
	$(BUILD)/ray_test

# testplayer: the pure-math AABB physics resolver (player.c) against a synthetic
# grid - landing, head-bonk, wall clamp, corner, anti-tunnel, water, free-fall.
# No GL, no platform, no world (same isolation as testraycast). -lm for ceilf/fabsf.
# apt: build-essential
testplayer: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/player_test $(SRC)/player.c $(SRC)/test_player.c -lm
	$(BUILD)/player_test

# testedit: world_get_voxel / world_edit_voxel on a headless WorldStore (sets the
# voxel, flags MODIFIED|DIRTY_MESH, dirties boundary neighbours). No GL/platform.
# apt: build-essential
testedit: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/edit_test $(SRC)/material.c $(SRC)/chunk.c $(SRC)/worldgen.c $(SRC)/world.c $(SRC)/persist.c $(SRC)/test_edit.c -lm
	$(BUILD)/edit_test

# 0.3 multiplayer: host+client over loopback in one process (net.c + the Linux
# socket backend). No GL, no world - pure protocol round-trip.
testnet: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/net_test $(NET) $(NET_LINUX) $(SRC)/test_net.c -lm
	$(BUILD)/net_test

# Create the build directory on demand (order-only prerequisite).
$(BUILD):
	mkdir -p $(BUILD)

# clean: remove all build artifacts.
clean:
	rm -rf $(BUILD)
