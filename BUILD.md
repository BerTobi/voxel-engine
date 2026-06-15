# Building the Voxel Engine

A single portable GL 2.1 / GLSL 1.20 engine core with a thin platform layer:
a **Win32 + WGL** backend (the real Windows XP target) and an **X11 + GLX**
backend (the Linux dev/test box). **Zero third-party libraries** — no
SDL/GLFW/GLEW; GL entry points are loaded by hand. See `ARCHITECTURE.md`
Appendix A (lines ~2055-2212) for the full rationale; this file is the
operational summary.

The deployment target is a 2005 Dell XPS M170: Pentium M 780 (Dothan, single
core, SSE2 only — **no SSE3+**), 32-bit Windows XP SP3, GeForce Go 7800 GTX
(G70, 256 MiB VRAM). The dev box is modern Linux with Mesa (4.6 compatibility
profile, a strict superset of GL 2.1). One core; two backends; one binary
shipped to XP.

---

## Versioning & releases

Factorio-style **`MAJOR.MINOR.PATCH`**, single source of truth in
`src/version.h`:

- **PATCH** (`0.1.0 → 0.1.1`) — bug-fix release, save-compatible.
- **MINOR** (`0.1 → 0.2`)     — feature release (new gameplay / systems).
- **MAJOR** (`→ 1.0`)         — the 1.0 release, or a deliberately breaking overhaul.

The version shows in the window title and a startup banner (`Voxel Engine 0.1.0`)
and is stamped into every save's region header (`game_version`) for forward
migration. `make version` prints it.

**Release process** (run for every version):

1. Bump the three numbers in `src/version.h`.
2. Add a dated, detailed entry to `CHANGELOG.md` (changes, by category).
3. `make archive` — builds BOTH targets and copies the executables to
   `archive/<version>/` (`voxel` + `voxel.exe`). `cp -n` never overwrites an
   already-archived version, so prior binaries are **permanently preserved**.
4. Commit, then tag the release: `git tag v<version>`.

The project is a **git repo** (constant backups). Tracked: `src/`, the `*.md`
docs, `Makefile`, and `archive/` + `screenshots/`. Git-ignored: `build/`
(regenerable) and `saves/` (runtime data) — see `.gitignore`.

---

## Milestone-1 status

**Tested and working** (pure C, no GL/OS dependency — `chunk.c`, `material.c`,
`mesher.c`, exercised by `test_mesher.c`):

- The 12-byte canonical vertex (`MeshVert`): `_Static_assert(sizeof == 12)` and
  `PER_VERTEX_BYTES == 12` both hold (case 1).
- Voxel codec: the two-segment round-to-nearest temperature codec
  (1 °C/step over −40..120 °C → codes 0..159; 20 °C/step over 120..2040 °C →
  codes 160..255) round-trips, including the Fe/Cu anchor cases (cases 6a/6b).
- Greedy mesher: empty chunk → 0 quads; solid 16³ chunk → 6 quads; half-filled,
  single-voxel, and mixed cases (cases 2-5).

The `test` target compiles with **zero warnings** under `-Wall -Wextra` and all
7 cases print `PASS` / `== ALL PASS ==`.

**Skeleton / not yet exercised:**

- `gl_loader.c` (manual GL function-pointer loader), `platform_linux.c`
  (X11+GLX), `platform_win32.c` (Win32+WGL), and `main.c` (the bring-up driver,
  with both `main()` for the console/Linux build and `WinMain` for the XP GUI
  subsystem) are written but compile only against the real GL/X11/win32 dev
  headers and have not been run on either backend yet.
- **`render.c` does not exist yet.** Only the contract in `render.h` is pinned.
  Because `main.c` calls `render_init / render_upload_chunk / render_begin /
  render_draw_chunk / render_end / render_shutdown`, the full-engine targets
  (`linux`, `win`) **will not link until `render.c` lands**. Everything else
  those targets need is in place. The `test` target is unaffected — it does not
  touch the renderer.

---

## Installing dependencies (Ubuntu)

The `test` target needs **nothing** beyond a host C compiler (`gcc`, already
present). Install the rest only for the build(s) you actually want:

```sh
# Linux dev/test build (X11 + GLX, Mesa GL headers):
sudo apt install libgl1-mesa-dev libx11-dev

# Windows XP cross-build (32-bit MinGW-w64 — the real deployment target):
sudo apt install gcc-mingw-w64-i686
```

That is the complete dependency list. No SDL, no GLFW, no GLEW — by design.

---

## Building and running each Makefile target

| Target         | Output            | Toolchain                  | Needs                       |
|----------------|-------------------|----------------------------|-----------------------------|
| `make test`    | `test_mesher`     | host `gcc`                 | nothing                     |
| `make linux`   | `voxel` (ELF)     | host `gcc`                 | `libgl1-mesa-dev libx11-dev`|
| `make win`     | `voxel.exe`       | `i686-w64-mingw32-gcc`     | `gcc-mingw-w64-i686`        |

### `make test` — the core unit tests (no deps, always runnable)

Builds and links `test_mesher.c` + `mesher.c` + `chunk.c` + `material.c` — the
pure-C core, no GL or windowing — into `test_mesher`, then run it:

```sh
make test
./test_mesher       # prints PASS lines and "== ALL PASS =="; exits 0 on success
```

This is the milestone-1 acceptance check and the fastest inner-loop feedback.

### `make linux` — the X11/GLX dev build

The portable engine core plus the `platform_linux.c` backend, the GL loader, and
`main.c`. Lets you fly the camera and render meshed chunks on the dev box
against Mesa (GL 2.1 compatibility subset).

```sh
make linux
./voxel
```

**Blocked until `render.c` exists** (see status above) — it will compile every
unit but fail at link with undefined `render_*` symbols.

### `make win` — the Windows XP cross-build (deployment artifact)

Cross-compiles the engine core plus `platform_win32.c` (Win32 + WGL) with
MinGW-w64 to a single static `voxel.exe`. The ISA, optimization, linking, and
stdio flags are pinned by `ARCHITECTURE.md` Appendix A:

- `-march=pentium-m -mtune=pentium-m -mfpmath=sse -msse2` — Dothan has SSE/SSE2
  only; **never** `-mavx/-msse3/-msse4/-march=native` (those `#UD` on the M170).
- `-O2` (not `-O3`), `-std=c99`, `-fno-strict-aliasing` (the `Voxel` u32
  type-punning), `-D__USE_MINGW_ANSI_STDIO=1` (conforming printf on XP's
  `msvcrt.dll`).
- `-static -static-libgcc -Wl,--gc-sections -mwindows` — no MinGW DLLs to ship;
  only `opengl32.dll` / `gdi32.dll` / `winmm.dll` stay dynamic (OS-owned, always
  present on XP). `ffast-math` stays **off** for sim determinism.

```sh
make win
# Deploy: copy the single voxel.exe to the M170 (scp / SMB share / USB) and run.
# No accompanying DLLs. make strip shrinks the release binary.
```

**Also blocked until `render.c` exists**, for the same link reason as `linux`.

### Supporting targets

```sh
make profile    # -pg build for gprof attribution (i686-w64-mingw32-gprof);
                # cross-reference against the in-engine QPC overlay, never trust
                # gprof's absolute ms (it perturbs the cache).
make debug      # -g -O0 unstripped build for debugging / Sleepy symbols.
make strip      # i686-w64-mingw32-strip voxel.exe — release size reduction.
make clean      # remove objects, .d files, and the built binaries.
```

---

## Notes

- Build on Linux, **judge on the M170**. The dev box compiles and runs the GLX
  build for convenience, but every performance claim is validated by the
  in-engine QueryPerformanceCounter overlay on the real XP hardware (the dev
  box's cache, clock, and ISA are nothing like Dothan).
- Incremental rebuilds come free from `-MMD -MP` header-dependency tracking; no
  hand-maintained dependency lists.
- C99 is both ceiling and floor: no VLAs, no `long double`, all formatted output
  through MinGW's ANSI stdio shim.
