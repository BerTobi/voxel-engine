# Changelog

All notable changes to the Voxel Engine. Versioning is Factorio-style
`MAJOR.MINOR.PATCH` (see `src/version.h`): pre-1.0 while the game takes shape;
`PATCH` = save-compatible fixes, `MINOR` = new gameplay/features, `MAJOR` = 1.0
or a breaking overhaul.

## 0.1.0 — 2026-06-15 — Tech-demo foundation

The complete runtime from the architecture document. This release is the
**engine foundation**: everything below works, cross-compiles to a single static
Windows XP `.exe` (and a Linux dev build), and is covered by 5 pure-C test
suites (81 tests). The next versions (0.2.0+) turn this foundation into an
actual game.

### Engine
- **Voxel core** — 4-byte voxel word (`mat | temp | fill | light | ao | flags`),
  16³ chunks, data-driven `MaterialDef` table (behaviour lives in the material,
  never the voxel).
- **Greedy meshing** — into a 12-byte interleaved vertex; **neighbour-aware**
  across cached `neigh[6]` so multi-chunk worlds have no seam walls.
- **GL 2.1 / GLSL 1.20 forward renderer** — manual GL loader (zero deps), 1024²
  material atlas, two-pass opaque + alpha liquids, baked lighting + AO, a live
  **day/night sun** (zero remeshes) and distance fog.
- **Cellular-automaton simulation** (fixed-point, single-threaded): FTCS heat
  diffusion, **melting/solidifying** with latent-heat banking, and **cellular
  fluid flow** (gravity + viscosity-gated lateral equalization) for water, lava
  and molten metal.
- **Streaming world** — a `WorldStore` (residency hash + slab pool) over
  deterministic procedural terrain, with a player-centred loaded window that
  generates ahead and evicts behind under a per-frame budget.
- **Persistence** — region files with palette+RLE compression; modified chunks
  survive eviction and a process restart (save-version stamped in the header).
- **Progression** — a read-only observer that turns emergent transitions into
  discoveries, an empirical material journal, and capability tiers (console
  journal; no in-world HUD yet).
- **Input** — FPS free-look (mouse turns, WASD relative to facing, Space/Shift
  up/down) on both the X11 and Win32 backends.

### Notes
- Single-chunk simulation; cross-chunk heat/fluid/light bleed is deferred.
- Progression is surfaced on the console (stderr) — an in-world HUD is planned.
- The lava demo holds heat only (it is not an inexhaustible spring) and is
  walled, so it stays a contained pool.
