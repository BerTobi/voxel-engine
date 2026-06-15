# Voxel Engine

A voxel survival/progression game in C + OpenGL where **mechanics emerge from
physics, not scripts**. Heat, fluids, and phase changes propagate through voxels
by their real material properties — a "furnace" isn't a coded block, it's a
stone cavity that gets hot enough to melt the ore inside it. Think *Noita's
cellular automaton, in 3D, at Minecraft scale*, with an industrial bent.

The twist: it targets a **2005 Dell XPS M170** — a single-core Pentium M, a
GeForce Go 7800 GTX (OpenGL 2.1), 32-bit Windows XP. The hardware constraint is
the design discipline. It cross-compiles to one **static `.exe`** (no DLLs) and
also runs natively on Linux for development.

**Status:** `0.1.1` — the tech-demo foundation (shipped as `0.1.0`), now with its
first round of bug fixes; gameplay starts next. See [`CHANGELOG.md`](CHANGELOG.md).

## What works (0.1.0)

- **Voxel core** — 4-byte voxel, 16³ chunks, data-driven `MaterialDef` table.
- **Renderer** — GL 2.1 / GLSL 1.20 forward renderer, greedy meshing (seamless
  across chunks), baked lighting + AO, a live day/night sun, distance fog,
  alpha-blended liquids. Zero third-party libraries.
- **Simulation** — single-threaded, integer fixed-point cellular automaton: FTCS
  heat diffusion, melting/solidifying with latent heat, and fluid flow
  (gravity + viscosity) for water, lava, and molten metal.
- **World** — a streaming, procedurally-generated world (WorldStore: residency
  hash + slab pool) with a player-centred loaded window, plus region-file
  **persistence** (edits survive eviction and restarts).
- **Progression** — a read-only observer that turns emergent transitions into
  discoveries + an empirical material journal (console for now).
- **Input** — FPS free-look: mouse to turn, WASD to move, Space/Shift up/down.

## Build & run

Dev box: Linux with `build-essential libgl1-mesa-dev libx11-dev` (and
`gcc-mingw-w64-i686` for the Windows cross-build). See [`BUILD.md`](BUILD.md).

```bash
make linux        # native dev build  -> build/voxel
make win          # static XP .exe    -> build/voxel.exe  (i686-w64-mingw32)
./build/voxel     # run: mouse looks, WASD moves, Esc quits

make test testsim testworld testpersist testprogress   # the 5 unit suites
make version      # print the version (from src/version.h)
make archive      # preserve both executables under archive/<version>/
```

## Layout

```
src/            engine source (C99): voxel/chunk/material, mesher, renderer +
                GL loader, platform_{linux,win32}, sim, worldgen, world, persist,
                progress, version.h, main.c
ARCHITECTURE.md the full design document (the source of truth for every decision)
BUILD.md        build, toolchain, and the versioning / release process
CHANGELOG.md    per-version history (Factorio-style MAJOR.MINOR.PATCH)
archive/<ver>/  preserved per-version executables
screenshots/    captured stills (lighting, fluids, streaming, day/night)
```

## Versioning

Factorio-style `MAJOR.MINOR.PATCH` (single source: `src/version.h`): **MINOR** =
feature release (`0.1 → 0.2`), **PATCH** = bug-fix release (`0.1.0 → 0.1.1`). The
version shows in the window title + startup banner and is stamped into save
files. Release process is documented in [`BUILD.md`](BUILD.md).
