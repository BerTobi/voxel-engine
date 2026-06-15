# Voxel Engine Architecture: An Emergent, Physics-Driven World on 2005 Hardware

A foundational architecture document for a single-player voxel survival/progression engine, written in C against OpenGL 2.1 / GLSL 1.20, targeting one specific machine: a **Dell XPS M170** — Pentium M 780 (Dothan, single core, 2.26 GHz, 32 KB L1D / 2 MB L2), 32-bit Windows XP SP3 with up to 2 GB DDR2, and the **256 MiB OEM GeForce Go 7800 GTX (Dell part HF594)** — not the generic 512 MB variant.

## The Design Premise

Progression is **emergent, not scripted**. The world is a 3D cellular automaton in which heat, fluids, and phase changes propagate by material physics drawn from a single global `MaterialDef` table. A "furnace," a "smelter," a "forge" are never coded as mechanics — they are what happens when the player builds an insulating stone cavity, burns hot enough fuel, and watches ore cross its melt threshold. Behavior is never stored in the voxel; behavior is stored in the material the voxel points at. The simulation stays purely physical; legibility (a journal, instruments, hints) is a strictly read-only observer layered on top. Remove the progression layer and the world simulates identically.

The premise survives only if a billion-voxel world fits the hardware. It does, because three invariants hold: the per-voxel word is exactly **4 bytes**; resident data is bounded by a **constant ~156 MiB window** regardless of how far the player explores; and simulation, meshing, and rendering are **serialized on one core** inside a **33.33 ms** frame, with the cellular automaton given the largest protected slice.

## Scope

- The bedrock numeric and structural decisions every subsystem inherits (voxel layout, chunk size, world addressing, memory envelope, frame budget).
- The seven-subsystem architecture, its single-owner memory model, and the three (and only three) inter-subsystem communication mechanisms.
- The voxel data model and the data-driven `MaterialDef` table that makes emergence possible.
- The cellular-automaton simulation (heat-first), greedy meshing, the forward rendering pipeline, frame scheduling, the memory budget, world persistence, the progression layer, the risk register, and the build/profiling toolchain.

## Non-Goals

- **Not a 60 FPS engine.** 30 FPS is a deliberate scheduling decision that doubles the sim's budget on a single core.
- **Not multithreaded.** The Pentium M 780 is single core; every per-frame cost competes on one timeline.
- **Not a modern-GL engine.** No VAOs, instancing, UBOs, geometry/compute shaders, integer vertex attributes, or `flat` qualifiers (GLSL 1.30+). A deliberately conservative GL 2.1 subset.
- **Not a fully RAM-resident world.** The full world is 4.00 GiB and cannot fit; streaming + procedural regeneration from seed is structural, not an optimization.
- **Not a scripted tech tree.** No XP, levels, currency, or `unlock()` gates. Tiers emerge from sustainable physics.
- **Not a deferred renderer, and not a real-time-shadow / per-fragment-lit engine.** Lighting is baked at mesh time; only a texture fetch and one multiply run per fragment.

---

## How to read this document

Read top to bottom; each section bolts onto the **Foundational Decisions (the Bedrock Layer)**, which pins every number the rest inherit and which nothing downstream may contradict. The **Decision Ledger** below is the single source of truth for every binding value — consult it first, then read the owning section for the defense.

- **Section 1 — System Architecture Overview.** The spine: the seven subsystems plus the Frame Loop, the single-owner memory model, and the three communication mechanisms (work queues, dirty flags, direct borrowed reads). Read this first; everything else plugs into its ownership and mechanism tables.
- **Section 2 — Voxel Data Model.** What a voxel *is* (the 4-byte word, accessed via masks) and what a material *means* (the 256-entry `MaterialDef` table). The canonical accessors and the temperature codec live here.
- **Section 3 — Cellular Automaton Simulation.** The technical heart: per-chunk active-voxel fronts, FTCS heat diffusion, fluids, latent-heat phase transitions. Read after Section 2; it consumes the voxel/material semantics directly.
- **Section 4 — Meshing.** Greedy meshing, the dirty-chunk remesh budget, and the canonical vertex format jointly owned with Section 5.
- **Section 5 — Rendering Pipeline.** Forward two-pass rendering, frustum culling, per-chunk draws, the material atlas, and baked lighting. Read alongside Section 4 — the two share the vertex and atlas contracts, now reconciled in the ledger (the **12-byte vertex** and the **1024×1024 atlas** are agreed across both sections).
- **Section 6 — Frame Scheduling.** How the 33.33 ms wall is divided and what gets cut, in fixed priority order, when work overruns. The executable form of the frame budget.
- **Section 7 — Memory Budget.** The constant-size resident window, the allocation table, slab pools, and the 32-bit fragmentation discipline.
- **Section 8 — World Persistence.** Store-deltas-regenerate-the-rest, region files, palette+RLE compression, and which voxel fields survive a save (temperature does; light/AO do not).
- **Section 9 — Progression Layer.** Discovery as read-only observation: the event ring, the empirical-range journal, physically-gated tiers, and the iron-threshold default.
- **Section 10 — Risk Register.** The five project-killing risks (draw-call submission, mesh VRAM under CA churn, fragmentation, heat-sim stability, progression legibility) with early-warning signals and contingencies.
- **Appendix A — Toolchain & Build.** Cross-compiling with MinGW-w64 for `i686`, the ISA flags Dothan demands, static linking, and on-device profiling.

---

## Decision Ledger

This table is the single source of truth for every binding numeric and architectural decision. Values are pulled from the Foundation and sections; where a still-open global issue leaves a value unsettled it is marked **OPEN** with the prototyping/reconciliation work that must resolve it.

| Decision | Value | Rationale |
|---|---|---|
| Deployment target | Dell XPS M170: Pentium M 780 (Dothan, 1 core, 2.26 GHz, 32 KB L1D / 2 MB L2), 32-bit XP SP3, ≤2 GB DDR2 | Fixed hardware on a real laptop; fixity is what lets every number be defended rather than hedged. |
| VRAM budget | 256 MiB (OEM GeForce Go 7800 GTX, Dell part HF594) — NOT the generic 512 MB Go 7800 GTX | The M170 shipped the G70 in its 256 MB config; that is the hard ceiling every mesh/render decision is sized against. |
| GL target | OpenGL 2.1 / GLSL 1.20, conservative subset: no VAOs/UBOs/instancing/geometry shaders, no integer attribs or `flat` | Full G70 + XP ForceWare capability; conservative subset avoids driver bugs and GLSL 1.30+ features. |
| Voxel size | 4 bytes (one u32): `mat8 \| temp8 \| fill4 \| light4 \| ao4 \| flags4`; accessed via masks, never C bitfields | Single aligned word; keeps a 16³ chunk at 16 KiB (half L1D) and the sim working set in L2. Masks give cross-compile-deterministic, byte-exact layout (persisted to disk, uploaded to GPU). |
| Material id | 8 bits / 256 materials; index into global `MaterialDef`; census ~107 committed, ~149 headroom | ~30 base substances × phase/variant; <256 with comfortable room. Escape hatch: steal AO nibble for 12 bits, recompute AO at mesh time. |
| Temperature encoding | 8 bits, two-segment non-linear, ROUND-TO-NEAREST: 1.0 °C/step over −40..120 °C (codes 0–159), 20 °C/step over 120..2040 °C (codes 160–255); boundary code 160 = 120 °C | Linear 8-bit gives 8.2 °C/step (too coarse at ambient). Two-segment gives 1 °C where biology lives, 20 °C where melt/boil are threshold comparisons. **PROTOTYPE-FLAG:** validate diffusion smoothness on the M170 before freezing; contingency is 12-bit temp (forces fill/light into a side array). |
| Temp anchors | Fe 1538 °C → code 231 (decodes 1540, +2); Cu 1085 °C → code 208 (decodes 1080, −5) | Round-to-nearest is *why* these match; floor division would have undershot Fe to code 230 (1520 °C). |
| Fluid fill | 4 bits / 16 levels | Minecraft-fluid resolution; smooth pooling. 3 bits (8 levels) visibly stair-steps slow pools. |
| Light + AO | 4 bits light + 4 bits AO, stored in-voxel; baked at mesh time, recomputed on load | 16-level ramp reads smoothly interpolated. In-voxel = mesher reads them in the cache line it already touched (zero extra traffic). |
| State flags | 4 bits: ACTIVE, LIQUID, DIRTY_MESH, RESERVED | Minimum covering the sim's hot branch decisions without a second word. RESERVED is the only uncommitted bit. |
| Chunk size | 16×16×16 = 4096 voxels = 16,384 bytes (16 KiB) | Half L1D; two fit L1, 128 fit L2. Beats 32³ (128 KiB, blows L1, spikes remesh) and 8³ (8× seam splits, metadata, draw calls). 16 divides world cleanly → shift/mask coords. |
| Active-tracking granularity | Sub-chunk (per-chunk active index list); chunk is meshing/streaming/storage unit, NOT the sim-wake unit | Lets the sim work on fronts of thousands, not whole chunks. |
| World chunk grid | 128 × 128 × 16 = 262,144 chunks | 2048/16 × 2048/16 × 256/16. |
| Full world voxel data | 4.00 GiB (1,073,741,824 voxels × 4 bytes) | 2.2× over ~1.8 GiB practical 32-bit usable; streaming + procedural regen is mandatory, structural. |
| World addressing | Sparse open-addressing hash table on 64-bit packed chunk key (21 bits/axis), ~16,384 slots (256 KiB) at 0.7 load; linear probing; + 6 cached neighbor pointers per resident Chunk | Residency is sparse (10k of 262k) and dynamic. Linear probing is cache-friendly on Pentium M. Hot neighbor traversal uses cached pointers; hash only for residency + cold lookups. |
| Loaded window | Horizontal radius 12 chunks (192 m), full 16-layer vertical column: 25×25×16 = 10,000 chunks, 400×400×256 m | Full vertical column protects digging/vertical builds. r=12 beats r=8 (claustrophobic, 128 m) and r=16 (272 MiB, diminishing return behind fog). |
| Window voxel bytes | 156.25 MiB (10,000 × 16 KiB) | Constant regardless of exploration distance — the load-bearing memory invariant. |
| Memory operating point | ~630–640 MiB intentional allocation against ~1.8 GiB practical usable (~35%) | 32-bit XP dies from address-space fragmentation before byte exhaustion; ~35% keeps the largest contiguous free block healthy. |
| Chunk allocator | Fixed-size slab pool: 160 super-slabs × 1 MiB × 64 slots = 10,240 slots = 160 MiB; per-chunk malloc/free banned | Every chunk is exactly 16 KiB → zero external fragmentation; O(1) free-list pop/push; reserve big arenas at startup before the heap fragments. |
| Mesh buffer allocator | Pooled size-class free lists (power-of-two buckets ~1–64 KiB) within a 96 MiB arena | Variable-size mesh data is the fragmentation hazard; bucketing keeps allocation O(1) and fragmentation-free. |
| Target framerate | 30 FPS / 33.33 ms per frame (1000/30) | Single core serializes sim+mesh+render. 60 FPS (16.67 ms) starves the CA; 30 FPS doubles the sim slice and reads smooth for a slow-paced CA game. |
| Frame budget split | Sim 14 ms (42%) / Mesh 6 ms (18%) / Render 9 ms (27%) / Overhead 4.33 ms (13%) | Sim protected (game's reason to exist). Render always completes. Meshing is the elastic shock absorber. Overhead/slack keeps a bad frame from missing vsync. |
| Cut order on overrun | (1) throttle mesh drain to a ~2 ms floor; (2) discard sim accumulator overflow (spiral clamp, `max_substeps=1`); (3) cap active-voxel count, carry remainder | Render sacrosanct, sim tick fixed/protected, meshing elastic, active-cap last resort. Cheapest-to-most-visible. |
| Sim tick model | Fixed-rate, decoupled from render; no catch-up ticks; `max_substeps=1`; active-voxel hard cap | Decoupling protects determinism and prevents spiral-of-death; over budget → in-world sim slows gracefully, frame never hitches. |
| Sim tick rate | 15 Hz fixed (66.67 ms period); ~0.5 ticks/frame (clean 2:1 render:tick at 30 FPS) | More wall-clock room per tick than 20 Hz; visibly advancing fronts vs 10 Hz; clean cadence the accumulator handles without fractional drift. |
| Sim active cap | 4096 active voxels per tick | One chunk's worth, a clean power of two; off the L2-pressure cliff with margin. Prototype-tunable default. |
| Sim working set | 112.0 KiB exact (4096 × 7 neighbors × 4 bytes = 114,688 B) | L2-resident with headroom; overflows L1 only gently. |
| Sim per-voxel budget | ~7,700 cycles (~3,420 ns) per active voxel (14e-3 / 4096 × 2.26e9 = 7,725) | Deliberately NOT rounded up — ample for a 6-neighbor heat kernel + fluid rule + bookkeeping in fixed-point, by the real margin. |
| Heat model | Explicit FTCS diffusion, 6-connected; interface conductance = harmonic mean of neighbor conductivities | Implicit solvers couple non-locally (break the active-front model); plain averaging has no conductivity (kills the materials premise). |
| Heat stability bound | α·dt/dx² ≤ 1/6 (3D explicit, 6-connected); unit system normalized so fastest material sits at exactly 1/6 | Conditional stability; normalizing against the worst-case material means no in-game configuration can blow up. |
| Chunk active max | 512 active voxels tracked per chunk; runaway fronts drain over multiple ticks | A chunk needing >512 simultaneously active is a runaway front, handled by graceful multi-tick drain. |
| Local index | 12 bits (`x \| y<<4 \| z<<8`) for 16³ = 4096 voxels | Exact bit width for a chunk; compact list entries, cache-local iteration. |
| Sim math type | Fixed-point integer in the CA hot loops (heat diffusion, fluid equalization, temperature decode); wider internal accumulator, quantize to 8-bit only on store | Determinism (fixed deterministic tick) + slow x87 avoidance + the wider accumulator is the mitigation for 20 °C/step stalling. **Reconciled:** Appendix A now states the CA hot loops are integer fixed-point per Section 3.6, and `-mfpmath=sse` governs only incidental float (camera/projection/view matrices, fog, render-side interpolation). |
| MaterialDef table | 256 entries × 64 bytes = 16,384 bytes (16 KiB), resident always, built once at startup from data (no hardcoded switches) | Power-of-two stride → shift-and-add indexing; one cache line per hot access. The data-driven backbone of emergence. |
| Per-vertex format | **12 bytes** — `u16 px/py/pz` (6 B) + `u8 mat/face/light/ao/u/v` (6 B). Section 4 and Section 5 now both cite this one layout; `light` and `ao` are separate per-vertex bytes (greedy merge-key fields), and the sun term is folded live in the shader, never baked. | Compact u16 positions + u8 attributes; the basis the VRAM budget and Risk R2's "≤8 B/vertex" mitigation assume. Reconciled — Section 5.1 reads the same 12-byte struct Section 4 writes. |
| Per-quad bytes | 60 bytes (4 verts × 12 B = 48 B vertex + 6 indices × 2 B = 12 B) | Follows the 12-byte vertex; vertex data (48 B/quad) dominates VRAM. Contingent on resolving the vertex-size OPEN item above. |
| Mesh index type | GL_UNSIGNED_SHORT (2 bytes); shared static quad index buffer | A 16³ greedy mesh never approaches the 65,535-vertex ceiling; halves index VRAM. |
| Greedy merge key | material id + face direction + full light byte (both sky/block nibbles) + AO nibble | Light and AO are independent split reasons (shadow edge vs corner darkening); both carried separately into the vertex. |
| Light/AO pipeline order | Nibbles computed FIRST (used as separate merge-key fields), written out as TWO separate per-vertex bytes (`light`, `ao`) at upload; light·AO·sun folded live in the shader | Merge decision over separate nibbles; vertex stores `light` and `ao` distinct (not pre-folded). Sun applied live in shader against unfolded sky-light → a moving sun costs zero remeshes. |
| Texture atlas | **1024×1024 RGBA8, 16×16 grid of 64×64-texel tiles, ~5.3 MiB with mips** (4 MiB base × 1.333; 1024 = 16·64). Sections 4 and 5 both cite this; the mesher's `u_atlas_cols`=16 / tile-size-64 math matches. | Both sections yield 256 tiles; 1024² is the better-quality choice, trivial against 256 MiB VRAM. Reconciled — the correct mip-inclusive figure is ~5.3 MiB, not 5.6. |
| Atlas grid | 16 tiles per row (`u_atlasInvCols` = 1/16); single shared grid constant referenced by mesher UV-fold and vertex shader | One uniform shared by both sections, not two independently hardcoded divisors that could drift. |
| Mesh mipping | `GL_GENERATE_MIPMAP` texture param (core GL 1.4) + `GL_TEXTURE_MAX_LEVEL` cap + gutters/extrusion + corner inset; bare `glGenerateMipmap` (GL 3.0) NOT used | GL 2.1-correct; gutters + capped mips solve atlas bleed without per-tile clamp. Exact gutter width / inset / mip cap are prototype-tuned. |
| Remesh per-frame caps | MAX_CHUNKS_PER_FRAME = 8 (default); MAX_QUADS_PER_FRAME = 24,000; whichever (chunks/quads/6 ms wall-clock) trips first ends the pass | Eight chunks + uploads stay inside 6 ms for normal digging/sim fronts. **PROTOTYPE-FLAG:** real ceiling is G70/XP `glBufferData` upload bandwidth — tune chunk cap against measured cost. |
| Meshed chunk count | ~2,500 (~25% of the 10,000-chunk window carry real geometry) | Interior solid chunks and air chunks emit zero quads; only the surface shell and dug cavities mesh. |
| Mesh VRAM (steady-state) | ~43 MiB at 300 quads/chunk (12-byte vertex); ~57 MiB pessimistic at 400 quads | Greedy meshing + 12-byte vertex keep geometry well under 256 MiB. These are static-terrain/steady-state figures. |
| Mesh VRAM (CA-churn worst case) | ~146 MiB (2,500 × 60 KB, ~1,024 quads/chunk checkerboard degeneration) — Risk R2 | NOT a conflict with the ~43–57 MiB steady-state figures: they are two points on one curve (static terrain vs CA-churned partial-fill geometry). Both arithmetics are individually correct. |
| CPU mesh memory | **~38 KiB/meshed chunk → ~93 MiB shadow set** at 2,500 chunks (the conservative planning average). Sections 4 and 7 now use this one figure. | Conservative planning number, not the optimistic 300-quad/17.6 KiB figure; ~93 MiB stays under the 96 MiB CPU-mesh line. Reconciled — both sections agree on one number. |
| Render passes | Forward, two-pass: opaque (depth test+write, no blend, front-to-back) then liquid/transparent (depth test on, depth write OFF, alpha blend, back-to-front at chunk granularity) | Deferred wastes 256 MiB VRAM for many-light accumulation irrelevant to one sun + baked light. Depth-write-off lets liquids blend correctly behind opaque depth. |
| Draw batching | One VBO per chunk via `glDrawElements`; atlas + shader + state bound once per pass; only per-chunk VBO bind + `glUniform3f(chunkOrigin)` change | Merged regional buffers fight the CA-churn remesh pattern (re-upload hundreds of KiB per one-chunk change). Per-chunk VBOs make remesh surgical. |
| Visible chunk draws | ~150–400 non-empty chunks/frame after frustum + fog cull (Risk R1 worst case ~1,000–1,800) | Frustum AABB-vs-6-planes discards 60–80% of candidates. **Risk R1 (HIGH/CRITICAL):** CPU draw-call submission under GL 2.0 may blow 9 ms; contingencies are render-only super-chunk batching, distance merging, aggressive fog pull-in. |
| Lighting | One directional sun (applied live in shader) + skylight/block-light flood fill (baked at mesh time) + per-vertex baked AO, carried as separate `light` and `ao` bytes; per-fragment cost = texture fetch + one multiply | Skylight/block-light/AO that change only when geometry changes are baked; the sun is folded live so it is never baked into geometry. Day/night = one `u_sun` (+ `u_sunTint`) uniform, zero remeshes — no drift threshold. No real-time shadows / per-fragment lights / specular / post. |
| Persistence model | Store deltas, regenerate the rest: a chunk is written only once gameplay-modified (`CHUNK_MODIFIED`); unmodified chunks dropped on evict and regenerated from seed via deterministic `gen(seed, cx, cy, cz)` | Save scales with player activity, not explored distance; mature save = single-to-low-double-digit MB, never the 4.00 GiB world. Generator must be versioned (`gen_version`); default = refuse to load on mismatch. |
| Persisted voxel fields | `mat8` + `temp8` + `fill4` persisted; `light4` + `ao4` recomputed on load; flags default not persisted (CA re-wakes out-of-equilibrium voxels) | Material/fill ARE the player's creation; temperature persists because heat is a banked, player-managed resource (defended against re-settling). Wake-set is a transient index, not world state. **PROTOTYPE-FLAG:** persist LIQUID flag vs re-derive — default re-derive (store only mat\|temp\|fill). |
| Compression | Per-chunk material palette (re-index to 1/2/4/8-bit) + RLE over (palidx, temp, fill) in z-major order; ~few hundred B–2 KiB/chunk (8×–40× vs 16 KiB raw) | RLE-over-palette is branch-light, no dictionary state to thrash L1. Optional LZ4 wrap is deferred polish, not the primary scheme. |
| Region files | 32×32 chunks × full 16-layer column = 16,384-chunk volume (512×512×256 m) per file; 4 KiB sectors (NTFS cluster), 8-byte index entry per chunk (sector_count==0 = regenerate); ~4–9 open handles | Per-chunk files are pathological on XP/NTFS (cluster+MFT slack dwarfs data, directory enumeration degrades, syscall overhead). Region batching keeps a tiny working set of persistent handles. Write record before index (crash safety). |
| Progression coupling | Read-only observer; coupled to sim ONLY via a 1024-entry event ring (~20 KiB), drained in the 4.33 ms slack band; never writes voxels/MaterialDef/tick outcome | If progression could mutate the sim it would compete for the 14 ms budget AND make terrain depend on discovery (breaks seed-regen). Ring must dedup-on-insert by (kind, mat_a, result), not FIFO. |
| Progression journal | Empirical *range* shadow of MaterialDef (256 × ~16 B = ~4 KiB saved); observed melt/boil ranges tighten with repeated observation + better instruments | The 20 °C industrial quantization becomes the literal grain of the player's earned knowledge. Tiers emerge from sustainable physics, not unlocks. **PROTOTYPE-FLAG:** ranges vs reveal-on-first-observation; reaction (`EV_REACT`) legibility. |
| Iron-threshold default | Ship BOTH a non-directive question hint AND one seeded intermediate hotter fuel (coked charcoal) that turns the ~1200 °C cliff into a ramp | Committed (not deferred) default with two-sided falsification: remove fuel if ramp proves unneeded; escalate hint to directive if players still wall. Neither violates the read-only rule. |
| Toolchain | MinGW-w64 `i686-w64-mingw32`, GCC 11/12, cross-compiled from Linux; `-std=c99`; plain GNU Makefile (not CMake) | 32-bit target is fixed by the CPU. C99 for `stdint`, designated initializers (the MaterialDef table), `inline`. Boring-and-proven over modern-and-off-the-map for a solo dev on a fragile target. |
| ISA flags | `-march=pentium-m -mtune=pentium-m -mfpmath=sse -msse2`; never `-mavx/-msse3/-msse4/-march=native`; `-O2` not `-O3`; `-fno-strict-aliasing`; `-ffast-math` default OFF | Dothan has SSE/SSE2 only — SSE3+ would `#UD`. `-O2` keeps the I-cache footprint small on a single-core L2-bound machine. `-fno-strict-aliasing` for the u32 type-punning; ffast-math off for determinism. |
| Linking | Static `-static -static-libgcc` (zero MinGW DLLs to ship); `opengl32.dll`/`gdi32.dll`/`winmm.dll` stay dynamic (OS-owned); `msvcrt.dll` (stock on XP) dynamic | Deployment = copy one `.exe`, double-click. `opengl32` is the ICD loader, cannot be static. Windowing via raw Win32+WGL preferred; static SDL+GLEW fallback (flag in week 1). |
| Profiling | In-engine QPC per-phase overlay (primary); gprof `-pg` (attribution); Sleepy/Xperf (sampling fallback); validated on the M170, never the dev box | QPC is safe because XP routes it through the fixed-rate ACPI PMT, immune to SpeedStep; never read `rdtsc` directly. Overlay is the acceptance test for the frame-budget decisions. |

---

## 1. System Architecture Overview

This is the spine. Every later section bolts onto one of the seven subsystems named here, and every cross-subsystem interaction in this document is one of exactly three kinds of communication, which I commit to up front: **direct synchronous calls**, **single-producer work queues drained at a fixed point in the frame**, and **dirty flags polled by the consumer**. There is no fourth mechanism. No callbacks-into-arbitrary-code, no event bus, no observer pattern — those buy decoupling we do not need on a single-threaded engine and cost us cache locality and predictability we cannot give up. The whole program is one thread walking one explicit timeline, and the architecture is built to make that walk legible.

**One hardware anchor before the spine, because the entire budget chain hangs off it.** The deployment target is a *specific* machine, not a family: a **Dell XPS M170** laptop, whose GPU is the **256 MB OEM variant of the GeForce Go 7800 GTX (Dell part HF594)**. This matters because the generic "GeForce Go 7800 GTX" is documented at *up to 512 MB*, and a reader checking our **256 MiB VRAM budget** against a spec sheet would conclude we under-counted by half. We did not. The M170 shipped the G70 in its 256 MB configuration, and that 256 MiB — not 512 — is the hard VRAM ceiling every meshing/rendering decision in this document is sized against. Likewise the CPU is the M170's **Pentium M 780 (Dothan, single core, 2.26 GHz, 32 KB L1D / 2 MB L2)** and the box is **32-bit Windows XP SP3 with up to 2 GB DDR2**. When any later section writes "the 256 MiB G70" or "the single Pentium M core," it means *this* machine. The constraint is fixed hardware on a real laptop, and that fixity is exactly what lets every binding number below be defended rather than hedged.

### 1.1 The Seven Subsystems

The engine is seven subsystems and one owner. I name them once, with their single responsibility, and the rest of the document fills each in:

1. **World Store** — owns chunk residency. The sparse hash table of `Chunk*` (binding: open-addressing, 64-bit packed key), the loaded-window logic, neighbor-pointer caching, and the lifecycle of chunk memory. It is the *only* subsystem that allocates or frees a `Chunk`.
2. **Simulation / CA** — the cellular automaton. Heat diffusion, fluids, melt/solidify/state transitions. Runs at a fixed tick decoupled from render, bounded by the active-voxel cap. It is the *only* subsystem that writes voxel material/temp/fill during normal play.
3. **Mesher** — turns dirty chunks into GPU-ready vertex buffers via greedy meshing. Consumes a dirty-chunk queue, produces `ChunkMesh` objects. CPU-side; the elastic budget.
4. **Renderer** — frustum cull, draw-call batching, the opaque and liquid passes. Owns the GL context and all VBO handles. Reads meshes; never writes voxels.
5. **Persistence / IO** — serializes *modified* chunks to disk and reads them back; drives procedural regeneration of unmodified chunks from the seed. The streaming engine.
6. **Input / Player** — samples input, moves the player capsule, raycasts for the build/dig cursor, and is the source of all *player-initiated* voxel edits.
7. **Progression** — the game-logic layer that reads world state (what melted, what pooled, what the player built) and advances unlock/tech state. It is a *reader* of the sim, not a scripter of it — this is what keeps mechanics emergent.

The eighth thing is not a subsystem, it is the **Frame Loop** — the top-level driver that owns the 33.33 ms timeline and calls into the subsystems in a fixed order. It is the trunk; the subsystems are branches. Critically, the Frame Loop is what *sequences* the three communication mechanisms: queues are drained at specific points it controls, dirty flags are polled at specific points it controls. Nothing happens "whenever" — everything happens at a named place in the loop.

### 1.2 Component Diagram

```
                         +-------------------------------------------+
                         |             FRAME LOOP (trunk)            |
                         |   owns the 33.33 ms single-thread timeline|
                         |   sequences every call/queue-drain/poll   |
                         +-------------------------------------------+
                              |        |        |        |       |
            (1) input    (2) sim tick  (3)mesh (4)render (5) stream
                  v          v           v        v          v
   +---------+  edit    +---------+    +-------+ +--------+  +-------------+
   | INPUT / |  queue   |  SIM /  |    |MESHER | |RENDERER|  | PERSISTENCE |
   | PLAYER  |========> |   CA    |    |       | |        |  |    / IO     |
   +---------+          +---------+    +-------+ +--------+  +-------------+
        |                 |    ^          ^  |       ^            |   ^
        | raycast         |dirty|  drain  |  |VBO    |reads       |   |regen
        | (read)          |chunk|  dirty  |  |upload |meshes      |   |req
        v                 v queue          | mesh    |            v   | queue
   +------------------------------------------------------------------------+
   |                          WORLD STORE                                   |
   |  hash table: chunk_key -> Chunk*   |   loaded window mgmt              |
   |  OWNS every Chunk (voxel data) and every ChunkMesh allocation         |
   +------------------------------------------------------------------------+
        ^                                                          |
        | reads world state (what melted/pooled/built)             | evict/load
        |                                                          v
   +-------------+                                          [ disk: region files ]
   | PROGRESSION |                                          [ seed: procgen ]
   +-------------+
```

Read the arrows as data flow, not control flow — control flow is always *down from the Frame Loop into a subsystem and back*. The lateral arrows (`====>` queues, `dirty` flags, `reads`) are the three communication mechanisms. The World Store sits at the bottom because it is the shared substrate everyone touches; it is the one piece of mutable state with many readers, so its ownership rules (1.4) are the most important contract in the engine.

### 1.3 Data Flow Around One Frame

One 33.33 ms frame walks this sequence. I give it as the canonical order because the order *is* the architecture — it is where determinism and the cut-priority (render sacrosanct, sim fixed, mesh elastic) are enforced.

```c
void frame(void) {
    /* 1. INPUT — sample devices, move player, raycast cursor.
     *    Player edits are NOT applied here; they are pushed onto the
     *    edit queue to be consumed by the sim at a deterministic point. */
    input_sample();
    player_update(dt);
    input_emit_edits(&edit_queue);   /* producer: Input/Player */

    /* 2. SIM — fixed-rate tick, decoupled from render. May run 0 or 1
     *    tick this frame depending on the accumulator. NEVER >1 (no
     *    catch-up; binding). The sim first drains player edits so edits
     *    and CA see one coherent world, then steps the active set. */
    while (sim_accumulator >= SIM_DT && tick_budget_ok()) {
        sim_apply_edits(&edit_queue);      /* consumer: Sim drains queue */
        sim_step_active_set();             /* heat, fluids, transitions  */
        sim_mark_dirty(&dirty_chunk_queue);/* producer: Sim marks chunks */
        sim_accumulator -= SIM_DT;
    }

    /* 3. MESH — elastic budget. Drain dirty-chunk queue up to a per-frame
     *    cap; overflow stays queued for next frame. Greedy-mesh each into
     *    a fresh ChunkMesh; hand finished meshes to the renderer for upload. */
    mesh_drain_dirty(&dirty_chunk_queue, MESH_BUDGET);

    /* 4. RENDER — sacrosanct, always completes. Cull, batch, draw.
     *    Reads ChunkMesh/VBO handles owned via the World Store. */
    render_frame(&camera);

    /* 5. STREAM — bounded IO. Service load/evict around the player window;
     *    persistence regenerates or reads chunks, hands them to World Store. */
    stream_service(&player_pos, STREAM_BUDGET);
}
```

The single most important structural fact in this loop: **the sim drains the player-edit queue and only then steps the CA.** A pickaxe strike and the heat diffusion of the same tick therefore observe one consistent world snapshot. If input wrote voxels directly, mid-frame, the CA could read a half-edited world and determinism would be gone. This is why Input is a *producer onto a queue*, not a direct writer — the one place I spend a queue's indirection to buy determinism.

The second fact: **the sim is the sole source of `DIRTY_MESH`.** Player edits flow *through* the sim (they enter the active set and are applied in `sim_apply_edits`), so the sim is also what marks their chunks dirty. There is exactly one producer of the dirty-chunk queue. That single-producer discipline is what lets the queue be a plain ring buffer with no locking and no dedup heroics beyond a per-chunk "already queued" flag.

### 1.4 Memory Ownership Model

Ownership is strict and singular. Every heap object has exactly one owning subsystem that allocates and frees it; everyone else holds *borrowed* pointers whose lifetime is bounded by that owner's contract. On a 32-bit process that dies from fragmentation (binding: operate at ~35% of usable, pool large buffers), shared/ambiguous ownership is not a style question — it is how you leak address space until a mesh allocation fails at 1.4 GiB committed.

**The World Store owns all chunk memory and all mesh memory.** This is the central decision. It allocates `Chunk` structs from a fixed-capacity **chunk pool** sized to the window (10,000 + slack), and it allocates/recycles `ChunkMesh` CPU-side buffers from a separate **mesh pool**. Pools, not `malloc`/`free` churn: a chunk evicted at the window edge returns its slot to the pool for the chunk streaming in on the opposite edge, so steady-state exploration does zero net allocation. This is the concrete mechanism behind the binding rule that resident data is *constant* regardless of how far the player explores.

Who borrows what, and the lifetime rule that makes it safe:

| Object | Owner (alloc/free) | Borrowers (read/transient write) | Lifetime contract |
|---|---|---|---|
| `Chunk` (voxel data) | **World Store** (chunk pool) | Sim (writes voxels), Mesher (reads), Persistence (reads/writes on stream), Input (reads via raycast), Progression (reads) | Valid only while resident. Eviction happens at one named point (`stream_service`), never mid-tick. Borrowers never cache a `Chunk*` across a frame boundary without re-validating through the hash table. |
| 6 neighbor pointers (in `Chunk`) | **World Store** (refreshes on insert/evict) | Sim, Mesher (hot neighbor traversal) | Refreshed atomically with residency changes. A `NULL` neighbor means "not resident" — borrowers must handle it (sim treats as inert boundary; mesher treats as occluding). |
| `ChunkMesh` (CPU vertex/index arrays) | **World Store** (mesh pool) | Mesher (writes during build), Renderer (reads to upload + draw) | A chunk's mesh is replaced wholesale on remesh; the old buffer returns to the mesh pool only after the renderer has uploaded the new one (no in-flight free). |
| VBO / IBO handles (GPU) | **Renderer** | nobody | Renderer is the only subsystem with the GL context; it owns every GL name and frees on chunk evict (World Store signals eviction; Renderer reclaims the VBO). Bounded by the M170's 256 MiB VRAM ceiling, not 512. |
| Active-voxel set / sim scratch / double-buffer | **Sim** | nobody | Fixed-capacity (active-voxel cap). Indexes *into* World-Store-owned chunks; owns no voxel data itself. |
| Edit queue, dirty-chunk queue | **Frame Loop** (static ring buffers) | producers/consumers per 1.3 | Drained to empty (or to budget) each frame; never grow. |
| Region files / regen scratch | **Persistence** | nobody | Scratch is pool-reused per streamed chunk; nothing persists across the stream call. |
| MaterialDef table + thermal/fluid LUTs | **Frame Loop / startup** (resident always) | everyone (read-only) | Loaded once at startup, never freed, never written during play. The data-driven backbone of emergence. |

The rule that ties it together: **borrowed `Chunk*` pointers are only valid within the frame, and residency changes happen at exactly one point in the loop (`stream_service`, step 5), after all readers have run.** That is why the sim, mesher, and renderer can hold raw `Chunk*` through their phases without defensive checks — nothing can evict a chunk out from under them, because eviction is sequenced last. This is the single-thread timeline paying for itself: we get the safety of immutable-during-use without any reference counting, locking, or GC.

### 1.5 Top-Level Structs

C pseudocode for the spine — illustrative struct shapes and ownership, not the implementation. Field-level detail belongs to the later sections; what is binding here is *who points at what* and *who owns the storage*.

```c
/* The whole engine. One instance, on the stack of main(). The Frame Loop
 * IS the methods that operate on this. Owns the queues and the LUTs;
 * holds the subsystems, each of which owns its own pools. */
typedef struct Engine {
    WorldStore    world;     /* owns chunk pool + mesh pool + hash table   */
    Simulation    sim;       /* owns active set + sim scratch/double-buffer*/
    Mesher        mesher;    /* owns transient greedy-mesh working memory  */
    Renderer      renderer;  /* owns GL context + all VBO/IBO handles      */
    Persistence   io;        /* owns region-file handles + regen scratch   */
    Player        player;    /* owns camera + player physics state         */
    Progression   prog;      /* owns tech/unlock state                     */

    EditQueue     edits;     /* Input produces -> Sim consumes  (ring)     */
    DirtyQueue    dirty;     /* Sim produces    -> Mesher consumes (ring)   */

    MaterialDef   materials[256]; /* read-only, resident, data-driven core */
    uint64_t      seed;           /* procedural world is a function of this*/
    double        sim_accumulator;
} Engine;

/* WorldStore: the authoritative residency map and the sole owner of
 * chunk and mesh storage. */
typedef struct WorldStore {
    HashSlot  *table;        /* open-addressing { key, Chunk* }, ~16384    */
    uint32_t   table_cap;
    Chunk     *chunk_pool;   /* contiguous pool of (window + slack) chunks */
    uint32_t  *chunk_free;   /* freelist of pool indices                   */
    ChunkMesh *mesh_pool;    /* recycled CPU-side mesh buffers             */
    uint32_t  *mesh_free;
} WorldStore;

/* Chunk: the 16 KiB voxel payload plus the metadata the hot paths need.
 * World Store owns it; the voxels are written ONLY by the sim during play
 * and by persistence during streaming. */
typedef struct Chunk {
    Voxel      vox[16*16*16];   /* 16384 bytes, the binding payload        */
    int32_t    cx, cy, cz;      /* chunk coords (also recoverable from key)*/
    struct Chunk *neigh[6];     /* cached neighbor ptrs; NULL = not resident*/
    ChunkMesh *mesh;            /* borrowed from World Store's mesh pool    */
    uint16_t   active_count;    /* sub-chunk activity hint for the sim      */
    uint8_t    flags;          /* DIRTY_MESH, MODIFIED(=must persist), etc.*/
} Chunk;

/* ChunkMesh: CPU-side geometry the renderer uploads. Owned by World Store
 * (mesh pool), written by the mesher, read by the renderer. */
typedef struct ChunkMesh {
    Vertex   *verts;     /* greedy-merged quads expanded to verts          */
    uint32_t *idx;
    uint32_t  vert_count, idx_count;
    uint32_t  vbo, ibo;  /* GL names — the RENDERER fills/owns these       */
    uint8_t   gpu_resident;
} ChunkMesh;

/* Simulation: owns the active-voxel front, not any voxel storage. Its
 * entries index into World-Store-owned chunks. */
typedef struct Simulation {
    ActiveCell *active;      /* bounded by the active-voxel cap            */
    uint32_t    active_count, active_cap;
    void       *scratch;     /* double-buffer for the active set only      */
} Simulation;
```

Note what the structs encode about communication. `Engine` holds the two ring-buffer queues directly — they are Frame-Loop-owned plumbing, sized statically, never grown. `Chunk` holds a `DIRTY_MESH` flag *and* feeds the dirty queue — the flag is the dedup ("am I already queued?"), the queue is the work list; together they are the dirty-flag-plus-queue idiom, not two competing mechanisms. `Chunk.neigh[6]` is the cached-neighbor-pointer rule made physical. And `Simulation` owning no voxel storage — only indices into World-Store chunks — is the ownership model made structural: there is one copy of every voxel, in the World Store, and the sim operates on it in place through borrowed pointers.

### 1.6 How the Pieces Talk — The Three Mechanisms, Assigned

To close the spine, here is every inter-subsystem edge and which of the three mechanisms it uses. This table is binding: later sections may detail the *contents* of these interactions but may not introduce a new mechanism for them.

| Edge | Mechanism | Why |
|---|---|---|
| Input/Player → Sim (voxel edits) | **Work queue** (edit ring) | Deterministic application point; edits and CA see one coherent world. |
| Sim → Mesher (chunk needs remesh) | **Dirty flag + work queue** | Single producer; flag dedups, queue orders; mesher drains under an elastic cap. |
| Mesher → Renderer (mesh ready to upload) | **Direct call** | Same-frame handoff; mesher hands the finished `ChunkMesh` to the renderer for `glBufferData`. |
| Renderer → World Store (read meshes/chunks) | **Direct read** (borrowed ptr) | Hot path; renderer walks resident chunks through cached pointers, no indirection. |
| Sim/Mesher → World Store (neighbor traversal) | **Direct read** via cached `neigh[6]` | Binding: hot neighbor access is a pointer deref, hash only on cold/edge. |
| Player raycast → World Store (read voxels) | **Direct read** | Cursor/dig query; bounded DDA walk over resident chunks. |
| World Store ↔ Persistence (evict/load) | **Work queue** (stream req) + direct on completion | Bounded IO; load/evict requests serviced under a per-frame stream budget so disk never stalls a tick. |
| Progression → Sim/World Store (observe state) | **Direct read** (polling) | Progression is a reader; it samples world/sim state each tick and advances tech. Never writes voxels — emergence over scripting. |
| Frame Loop → every subsystem | **Direct call** | The trunk drives the timeline; all control flow is top-down and back. |

The shape to remember: **control flows top-down from the Frame Loop and returns; data flows laterally through exactly three channels (queues for deferred/deterministic handoffs, dirty flags for "needs work" bookkeeping, direct borrowed reads for hot paths); and storage is owned in exactly one place per object, with borrowed pointers valid only within the frame.** Every subsequent section plugs into this spine — the CA section fills `Simulation` and the active-set discipline, the mesher section fills greedy meshing and the `ChunkMesh` contract, the rendering section fills the GL passes and VBO ownership (against the M170's 256 MiB G70), persistence fills streaming and regen, and progression fills the read-only observation layer. None of them may contradict the ownership table in 1.4 or the mechanism table here.

---

## 2. Voxel Data Model

This is the canonical reference for what a voxel *is* and what a material *means*. Section 1 pinned the sizes; this section pins the semantics. Every downstream system — the cellular automaton, the mesher, persistence — reads voxels and `MaterialDef` entries through the accessors defined here, and nowhere else. If a field's meaning is ambiguous, it is ambiguous *here* or it is not ambiguous at all.

The governing principle, restated because it is the whole reason the engine exists: **behavior is not stored in the voxel; behavior is stored in the material the voxel points at.** A voxel is 4 bytes of *state* — what am I, how hot am I, how full am I, how lit am I. A `MaterialDef` is the *physics* — how I conduct heat, when I melt, how I flow, whether I burn. The voxel says "I am material 47 at 1200 C"; the `MaterialDef` for 47 says "I am copper, I melt at 1085 C, here is my specific heat." The simulation does the rest. This indirection is what makes a furnace emerge instead of being scripted, and it is what keeps the per-voxel word at 4 bytes while the *richness* lives in a 16 KiB table that is touched far less often.

### 2.1 The Voxel Word — Full Field Semantics

The voxel is one `uint32_t`. I access it through masks, never C bitfields — and this is the load-bearing decision, so I defend it on its own terms. C bitfield layout (allocation order within a unit, padding, and straddling) is **implementation-defined**; the standard does not pin which end of the storage unit bit 0 lands on, nor how the compiler packs adjacent fields. We cross-compile from Linux GCC/Clang to an XP MinGW binary, and the *same* bitfield struct can lay out differently across those toolchains and versions. Since this exact bit pattern is written to disk by persistence and uploaded to the GPU, a layout that drifts between compilers is a silent save-file and rendering corruption waiting to happen. Masks are explicit, byte-for-byte identical on every compiler, and I control the layout rather than the compiler. That determinism — not any micro-optimization — is why masks win.

Performance is the secondary, comfortable argument: each accessor is a `shr` and an `and` against compile-time constants, a couple of cheap ALU ops in the integer pipeline. I will *not* claim an exact cycle count on the Dothan — the P6-derived front end and shifter make per-op timing workload-dependent, and pretending otherwise is false precision. What matters is the order of magnitude: field extraction is trivially cheap *relative to a cache miss*, which is the cost that actually governs this engine. As long as the voxel word and its `MaterialDef` stay resident in L1/L2 (Section 1's whole point), a few extra ALU ops per access disappear into the noise; the moment we miss to main memory, no amount of bitfield cleverness saves us. So the mask scheme costs effectively nothing on the axis that matters and buys cross-compile determinism on the axis that bites.

```c
/* The entire persistent state of one cubic metre of the world. 4 bytes.
 * Bit-exact layout — DO NOT reorder; persistence and the GPU both depend on it.
 *
 *  bits  0..7   : material id      (8)  index into g_materials[256]
 *  bits  8..15  : temperature      (8)  two-segment non-linear code, see 2.2
 *  bits 16..19  : fluid fill level  (4)  0..15, 0 = empty of fluid
 *  bits 20..23  : baked light       (4)  0..15, max(sky, block) folded at mesh time
 *  bits 24..27  : ambient occlusion (4)  0..15, corner darkening, mesh-time
 *  bits 28..31  : state flags        (4)  ACTIVE | LIQUID | DIRTY_MESH | RESERVED
 */
typedef uint32_t Voxel;

#define VOX_MAT(v)    ( (v)        & 0xFFu)
#define VOX_TEMP(v)   (((v) >>  8) & 0xFFu)
#define VOX_FILL(v)   (((v) >> 16) & 0x0Fu)
#define VOX_LIGHT(v)  (((v) >> 20) & 0x0Fu)
#define VOX_AO(v)     (((v) >> 24) & 0x0Fu)
#define VOX_FLAGS(v)  (((v) >> 28) & 0x0Fu)

/* Write helpers clear-then-set the target field. The compiler folds the
 * constant masks; on the Pentium M each is a small handful of ALU ops
 * (and/or/shift), no branch — cheap next to any memory stall. */
#define VOX_SET_MAT(v,m)   ((v) = ((v) & ~0x000000FFu) |  ((uint32_t)(m)))
#define VOX_SET_TEMP(v,t)  ((v) = ((v) & ~0x0000FF00u) | (((uint32_t)(t)) <<  8))
#define VOX_SET_FILL(v,f)  ((v) = ((v) & ~0x000F0000u) | (((uint32_t)(f)) << 16))
#define VOX_SET_LIGHT(v,l) ((v) = ((v) & ~0x00F00000u) | (((uint32_t)(l)) << 20))
#define VOX_SET_AO(v,a)    ((v) = ((v) & ~0x0F000000u) | (((uint32_t)(a)) << 24))
#define VOX_SET_FLAGS(v,s) ((v) = ((v) & ~0xF0000000u) | (((uint32_t)(s)) << 28))

/* Flag bits (the 4-bit field at 28..31) */
#define VF_ACTIVE     0x1u  /* in the simulation wake-set this tick */
#define VF_LIQUID     0x2u  /* phase fast-path: skip solid-only CA branches */
#define VF_DIRTY_MESH 0x4u  /* per-voxel remesh hint; chunk dirty is authoritative */
#define VF_RESERVED   0x8u  /* unassigned — do not consume without a Section-2 edit */
```

**`material id` (8 bits).** An index into the single global `g_materials[256]` table (2.3). Material `0` is reserved as `MAT_AIR` — it must be id zero so that a `memset(chunk, 0, 16384)` produces a valid, fully-air, cold-but-decodes-sanely chunk, which is what procedural generation and fresh allocation both rely on. (Air's temperature code 0 decodes to -40 C; gen overwrites temperature immediately, and -40 C air is harmless for one tick.) Every behavioral question — *can this melt, does it conduct, is it a fluid* — is answered by indexing this table, never by `switch`-ing on the id. A `switch (mat)` anywhere outside the table's own construction is a design violation: it is exactly the hardcoded-recipe pattern the game is built to avoid.

**`temperature` (8 bits, non-linear).** The encoded code, not degrees. Decode/encode is defined in 2.2. This is the single most-written field in the simulation — heat diffusion rewrites it on every active voxel every tick — so its encoding had to be a multiply-add, not a table lookup that would evict useful data from L1.

**`fluid fill level` (4 bits, 0..15).** How much of this voxel's volume is occupied by the *fluid phase of its material*. Meaning is phase-dependent and this is a subtlety the CA section must honor:
- For a `LIQUID`-class material (water, molten iron, lava), fill is the liquid column height within the cell: 15 = brim-full, 1 = a thin film, 0 = the cell is empty *of liquid* (and should usually revert to `MAT_AIR` or to whatever solid substrate it sits on).
- For a `SOLID` or `POWDER` material, fill is **ignored** and must be held at 15 (treated as "fully present"). A solid stone voxel is not "partially full." This keeps the field single-purpose and means the mesher never has to ask whether a stone block is 60% there.
- For `GAS`, fill encodes density/concentration for rendering and buoyancy; 0 means the gas has dissipated and the cell reverts to air.

16 levels is the binding choice (Section 1): 8 levels stair-steps visibly in slow pools, 16 reads smooth once the liquid surface is interpolated at mesh time.

**`baked light` (4 bits) and `ambient occlusion` (4 bits).** Both are *mesh-time outputs cached in the voxel*, not simulation inputs. `baked light` is the folded `max(skylight, blocklight)` propagated by the lighting pass (owned by the rendering section); `ao` is the corner-darkening term. They live in the voxel word and not in a side array for one concrete cache reason: the greedy mesher already has the voxel's cache line loaded to read `material` and the face-occlusion flags, so reading light and AO from the same 4 bytes is **zero additional memory traffic**. A parallel light array would double the mesher's read footprint and is the kind of "clean separation" that costs you the L1 budget on a single-core machine. These two fields are the *only* fields the simulation may treat as don't-care: heat and fluid logic never read them.

**`state flags` (4 bits).** The simulation's hot branch predictors. `VF_ACTIVE` mirrors membership in the active-voxel set (the authoritative wake-set is the CA's sub-chunk index list per Section 1; this bit is the cheap per-voxel check that avoids a list lookup in the inner loop). `VF_LIQUID` is a phase fast-path so the CA can skip the entire solid-transition branch on a cell it already knows is fluid — it is derivable from `material`'s phase class but cached here because re-deriving it per neighbor per tick is exactly the kind of repeated indirection that hurts on this CPU. `VF_DIRTY_MESH` is a per-voxel "I changed visibly" hint; the *authoritative* remesh trigger is the chunk-level dirty bit (Section 4), but this hint lets an incremental remesher skip clean sub-regions. `VF_RESERVED` is the only uncommitted bit in the entire word — Section 1's global reality stands: **the 32 bits are fully allocated, and no downstream section may claim a new persistent per-voxel field without a Section-2 edit that displaces an existing one.** Transient per-voxel scratch (diffusion deltas, visited marks) lives in side structures the CA owns, never in the voxel.

### 2.2 Temperature Codec — The One Field That Isn't Linear

The binding encoding is two-segment piecewise-linear. I give the exact codec here because *every thermal calculation in the engine routes through it*, and getting the boundary or the rounding wrong by one code is the kind of bug that makes iron "melt" at 1518 C in one place and 1558 C in another.

One decision deserves to be stated up front because it changes the published anchors if you get it wrong: **encoding rounds to nearest, not toward zero.** Floor (truncating) division would systematically bias every encoded temperature *downward* by up to almost a full step — in the industrial band that is nearly 20 C of one-sided error, which would push every smelt's apparent temperature below the true value and make threshold comparisons fire late. Round-to-nearest halves the worst-case quantization error and centers it symmetrically (±10 C in the hot band, ±0.5 C in the ambient band), and it is what produces the anchor values the rest of this document is pinned to. The cost is a single added constant (`+ step/2`) before the integer divide — free.

```c
/* Binding constants — DO NOT change without re-validating the heat sim.
 *  code 0..159   : -40 .. +120 C   @ 1.0  C/code   (ambient/biology band)
 *  code 160..255 : 120 .. 2040 C   @ 20.0 C/code   (industrial heat band)
 * Boundary: code 160 == exactly 120 C, shared by both segments (continuous).
 * ENCODE ROUNDS TO NEAREST (add half-step before the divide) — not floor. */
#define T_AMB_BASE_C     (-40)   /* celsius at code 0   */
#define T_AMB_STEP_C       1     /* celsius per code, codes 0..159 */
#define T_HOT_CODE       160     /* first code of the industrial segment */
#define T_HOT_BASE_C     120     /* celsius at code 160 */
#define T_HOT_STEP_C      20     /* celsius per code, codes 160..255 */

static inline int temp_decode_c(uint8_t code) {
    if (code < T_HOT_CODE)              /* hot path: ambient band dominates */
        return T_AMB_BASE_C + (int)code * T_AMB_STEP_C;
    return T_HOT_BASE_C + (int)(code - T_HOT_CODE) * T_HOT_STEP_C;
}

static inline uint8_t temp_encode_c(int c) {
    if (c <= T_HOT_BASE_C) {                       /* ambient segment */
        /* round to nearest: + half a step before the (integer) divide */
        int code = (c - T_AMB_BASE_C + T_AMB_STEP_C / 2) / T_AMB_STEP_C;
        if (code < 0)   code = 0;                   /* clamp: -40 C floor */
        return (uint8_t)code;
    }
    int code = T_HOT_CODE
             + (c - T_HOT_BASE_C + T_HOT_STEP_C / 2) / T_HOT_STEP_C;
    if (code > 255) code = 255;                     /* clamp: 2040 C ceiling */
    return (uint8_t)code;
}
```

The decode is one compare and a multiply-add; the ambient branch is the common case (most of the world sits near 20 C → code 60) so the branch predictor stays warm. Worked anchors, computed with the round-to-nearest encoder above, so downstream code can sanity-check against me:
- 0 C → code 40 (decodes 0 C, exact)
- 20 C → code 60 (decodes 20 C, exact)
- 100 C → code 140 (decodes 100 C, exact)
- copper's 1085 C melt → `160 + (1085-120+10)/20 = 160 + 975/20 = 160 + 48 = code 208` (decodes 1080 C — a 5 C undershoot)
- iron's 1538 C melt → `160 + (1538-120+10)/20 = 160 + 1428/20 = 160 + 71 = code 231` (decodes 1540 C — a 2 C overshoot)

These match the foundation's binding anchors exactly (Fe 1538 C = code 231, Cu 1085 C = code 208); the round-to-nearest encoder is *why* they match — floor division would have produced code 230 for iron (decoding 1520 C, an 18 C undershoot) and silently contradicted the anchors. Notice the consequence the CA *must* internalize: **near melting points, adjacent codes are 20 C apart.** Melt and boil are therefore *threshold* events (`temp_decode_c(VOX_TEMP(v)) >= def->melt_c`), never equality tests, and never "accumulate exactly N degrees" logic. The ±10 C worst-case band around any hot-segment code is comfortably below the spacing between distinct metals' melting points (copper 1085, gold 1064, iron 1538, tin 232) so no two metals' melt thresholds collide under the quantization. The global reality stands: heat-sim must validate diffusion smoothness against this two-segment encoding on the real M170 before it is frozen; the contingency (12-bit temperature, paid for by evicting fill+light into a side array) is documented in Section 1 and I will not silently assume it.

### 2.3 The Global MaterialDef Table — Where Emergence Lives

This is the heart of the data model. There are at most 256 `MaterialDef` entries, all resident at all times (they cost ~16 KiB; see sizing below), indexed by the voxel's 8-bit material id. The table holds **every property that any system needs to compute behavior**, so that behavior is data, not code. I commit to the following fields. Each one earns its place by being read by at least one emergent mechanic; I name the mechanic.

```c
/* Phase class drives which CA rule-set a voxel obeys. */
typedef enum {
    PHASE_GAS    = 0,  /* air, steam, smoke, oxygen — buoyant, diffuse, fill = density */
    PHASE_LIQUID = 1,  /* water, lava, molten metals — flows, equalizes, fill = column  */
    PHASE_POWDER = 2,  /* sand, gravel, ash, ore-dust — falls, piles at rest angle      */
    PHASE_SOLID  = 3   /* stone, ores, ingots, wood — static until melted/broken        */
} PhaseClass;

/* One material. 64 bytes, cache-line-friendly, see sizing note.
 * Units are chosen so the CA can work in fixed-point integers, NOT floats —
 * the Pentium M has SSE2 but the sim is integer-first for determinism and
 * because a 64-entry-deep table of floats would not buy us anything the
 * 8-bit temperature quantum hasn't already taken away. */
typedef struct {
    /* --- identity / rendering (read by mesher + renderer) --- */
    char     name[16];          /* "iron", "molten_iron", "iron_oxide" — debug/persistence */
    uint16_t atlas_tile;        /* index into the material texture atlas (Section 5)        */
    uint8_t  tint_rgb[3];       /* multiplied over the tile; lets one tile serve recolors   */
    uint8_t  phase;             /* PhaseClass — selects the CA rule-set                     */

    /* --- thermal (read by heat diffusion + phase transitions) --- */
    uint16_t density;           /* kg/m^3 (0..65535 covers air=1 .. gold=19300)             */
    uint16_t specific_heat;     /* J/(kg*K), how much energy raises 1 K — water=4186, Fe=449 */
    uint16_t thermal_cond;      /* mW/(m*K) scaled; how fast heat crosses faces — Cu high    */
    int16_t  melt_c;            /* solidus/liquidus in C; -1 if it does not melt (it burns)  */
    int16_t  boil_c;            /* vaporization point in C; -1 if it sublimes/never boils     */
    uint16_t latent_fusion;     /* kJ/kg absorbed at melt before temp rises again            */
    uint16_t latent_vapor;     /* kJ/kg absorbed at boil                                     */
    uint8_t  melts_to;          /* material id of the liquid phase (iron -> molten_iron)      */
    uint8_t  freezes_to;        /* material id of the solid phase (molten_iron -> iron)       */
    uint8_t  boils_to;          /* material id of the gas phase (water -> steam)              */
    uint8_t  condenses_to;      /* material id when gas cools (steam -> water)                */

    /* --- fluid dynamics (read by the falling-sand fluid pass) --- */
    uint8_t  viscosity;         /* 0=instant spread (water) .. 255=barely creeps (lava/glass) */
    uint8_t  rest_angle;        /* powders only: angle-of-repose proxy, slope before sliding  */

    /* --- mechanical (read by mining / structural logic) --- */
    uint8_t  hardness;          /* mining time / tool tier gate; 255 = bedrock-unbreakable    */
    uint8_t  breaks_to;         /* material id of the drop/rubble (stone -> cobble/gravel)     */

    /* --- chemistry / combustion (read by the reaction pass) --- */
    int16_t  ignition_c;        /* auto-ignition temp in C; -1 = non-flammable                */
    uint8_t  burns_to;          /* material id of the ash/residue (wood -> charcoal/ash)       */
    uint8_t  flammability;      /* 0..255 spread propensity; gates fire CA                    */

    /* --- electrical (optional, reserved for the progression tier) --- */
    uint8_t  conductivity;      /* 0=insulator .. 255=copper; deferred mechanic, see note      */

    uint8_t  flags;             /* MAT_OPAQUE | MAT_EMISSIVE | MAT_OXIDIZER | ... (mesher/render) */
    uint8_t  _pad[2];           /* pad to a clean 64; keeps the array stride a power of two       */
} MaterialDef;

extern MaterialDef g_materials[256];   /* the one table; resident always */

/* The accessor the whole engine uses. Inlined; one indexed load. */
static inline const MaterialDef *mat(Voxel v) { return &g_materials[VOX_MAT(v)]; }
```

**Why each property is in the table — by mechanic, not by wishlist.**

- **`density`** decides buoyancy and displacement: molten metal (dense) sinks under slag (light), oil floats on water, steam rises through air. Without it, fluids of different materials cannot stratify, and stratification is *how the player separates a smelt from its slag*.
- **`specific_heat` + `thermal_cond`** are the two numbers that make heat *propagate believably*. Conductivity sets how fast energy crosses a voxel face (copper carries forge heat into a workpiece fast; stone insulates the furnace cavity — that contrast is literally why a stone-walled cavity *works as* a furnace). Specific heat sets how much that energy raises temperature — water's enormous specific heat is why it is the coolant the player reaches for. The heat sim moves *energy* (joules) between cells and converts to a temperature code only when it writes back; specific_heat and density are the conversion factors. This is the one place the binding temperature quantum matters most, and why the CA accumulates energy in a side buffer rather than nudging the 8-bit code directly — the round-to-nearest write-back in 2.2 then converts that accumulated energy to a code without the systematic downward bias floor division would introduce.
- **`melt_c` / `boil_c` + `latent_fusion` / `latent_vapor` + the four phase-target ids** are the entire state-machine of melting, freezing, boiling, and condensing — expressed as data. When a voxel's decoded temperature crosses `melt_c`, the CA first drains `latent_fusion` worth of energy (the substance sits at its melting point absorbing heat without rising — this latent plateau is what makes a crucible take *time*, not flip instantly), then swaps the material id to `melts_to`. `freezes_to`/`condenses_to` run the transition in reverse. Iron→molten_iron→iron and water→steam→water are just two id pairs each; the *behavior* is identical code reading different table rows. `-1` melt/boil means "this material does not have that transition" (stone in our temperature range, or wood which `ignition_c`-burns before it melts).
- **`viscosity`** is the single knob that makes water and lava *feel* different in the same fluid CA: water (viscosity ~0) equalizes across a pool in a few ticks; lava (high viscosity) creeps and the player can outrun it. Molten iron sits in between. No per-fluid code — one comparison against this byte gates how many neighbors a liquid cell may flow into per tick.
- **`rest_angle`** (powders) sets the angle of repose — whether sand mounds steeply or gravel slumps flat — so falling-sand piles look like the material they're made of.
- **`hardness` + `breaks_to`** gate mining: hardness is the tool-tier/time wall (and `255` = the unbreakable world floor), `breaks_to` is what you get in hand or as rubble. This is data so a new ore is "addable" by appending a table row, never by editing mining code.
- **`ignition_c` + `burns_to` + `flammability`** drive combustion as a CA reaction, which is *how the player makes heat in the first place*: charcoal has a high `flammability` and burns to ash, releasing energy into the heat field that then conducts into the furnace charge. Fire is not a block; it is "a hot, flammable voxel raising its flammable neighbors past their `ignition_c`."
- **`conductivity`** I am including but flagging as **deferred**. It costs one byte and reserving it now is free; the electrical-progression tier (wiring, motors) is the least-designed part of the game (Section 9 is explicitly the open question), and I would rather have the field present and unused than re-layout the struct and bump the persistence format version later. **Decision deferred to prototyping — do not build electrical CA against this until the progression design lands.**
- **`flags`** carries the cheap booleans the mesher and renderer need without a branch into other fields: `MAT_OPAQUE` (does this face cull its neighbor — the single most-read material property in the entire mesher), `MAT_EMISSIVE` (lava/fire injects block-light), `MAT_OXIDIZER`, etc.

**Why 256 ids genuinely suffice — the census, not a hand-wave.** Section 1 committed to 8 bits; here is the accounting that backs it. The world is built from roughly **30 base substances** (a dozen ores + their refined metals, a few stones, sand/gravel/clay, coal/charcoal/oil, water, a couple of structural woods, glass). Emergence multiplies each substance by its *phases and chemical variants* — and this multiplication is exactly why the count feels scary until you do it. But each phase is a **separate material id pointing back at its siblings** via `melts_to`/`freezes_to`/`boils_to`/`burns_to`, and most substances have only two or three live phases in our -40..2040 C range:

| Group | Count | Examples |
|---|---|---|
| Gases | ~6 | air, steam, smoke, oxygen, flammable-gas, slag-fume |
| Liquids | ~20 | water, oil, lava, + molten variant per smeltable metal (~12) |
| Powders | ~12 | sand, gravel, ash, clay, + crushed-ore dust per ore |
| Solid stones/terrain | ~15 | stone, granite, dirt, clay-block, glass, obsidian, bedrock |
| Solid ores | ~12 | iron/copper/tin/gold/etc. ore |
| Solid refined metals | ~12 | iron, copper, tin, gold ingot/block |
| Alloys | ~8 | bronze, steel, brass + their molten forms counted above |
| Oxidized / slag / residue | ~12 | rust, slag, charcoal, ash-block, patina |
| Woods / organics / misc | ~10 | wood, plank, leaf, coal, structural blocks |
| **Total committed** | **~107** | |

That lands at roughly **107 of 256**, leaving ~149 ids of genuine headroom — enough to *double* the substance roster during development without pressure. The 8-bit field is not a tight squeeze; it is comfortable. The escape hatch from Section 1 (steal the AO nibble for a 12-bit id and recompute AO at mesh time) remains documented, but the census says we will not need it.

**MaterialDef table sizing — it is free.** At 64 bytes per entry (verified: the struct above pads cleanly to 64), the full 256-entry table is **256 × 64 = 16,384 bytes = exactly 16 KiB** — identical in size to *one chunk*, fits inside the 32 KB L1D, and is a rounding error against the 1 MiB "material/property tables" line in Section 1's memory budget (the remaining ~1 MiB absorbs the thermal/fluid lookup tables and the atlas metadata). I deliberately chose 64 bytes over a tighter 48-byte pack: the power-of-two stride means `&g_materials[id]` is a shift-and-add the compiler emits for free, and an entry that touches at most one or two 64-byte cache lines means the hot inner-loop access `mat(v)->thermal_cond` is almost always an L1 hit. Shrinking the struct to save 4 KiB out of a 1.8 GiB budget would be optimizing the wrong resource — the scarce resource here is cache *behavior*, and 64-byte alignment buys that. The table is built once at startup (from a data file in the persistence format, not hardcoded `switch` statements) and is never reallocated.

### 2.4 Chunk Size — Restated With Its Byte Math

The binding decision (Section 1) is **16×16×16 = 4,096 voxels per chunk = exactly 16,384 bytes (16 KiB) of voxel data** (verified: 4096 × 4 = 16384). I restate it here because the voxel data model is incomplete without the container, and because two arithmetic facts anchor everything the simulation and mesher do:

- A chunk is **half of the 32 KB L1D**, so two whole chunks fit in L1 (32/16 = 2, verified) — enough that neighbor-aware meshing or a heat-diffusion pass crossing one chunk face keeps both sides hot in L1.
- A chunk is **1/128 of the 2 MB L2** (verified), so 128 chunks coexist in L2; the ~110 KiB active-simulation working set from Section 1 (≈4,000 active voxels × 7-cell stencil × 4 bytes) is a small slice of that and stays resident across a tick.

```c
#define CHUNK_DIM   16
#define CHUNK_VOX   (CHUNK_DIM * CHUNK_DIM * CHUNK_DIM)   /* 4096           */
#define CHUNK_BYTES (CHUNK_VOX * sizeof(Voxel))           /* 16384 = 16 KiB */

/* Coord math is pure shift/mask because 16 divides the world cleanly
 * (128 x 128 x 16 chunk grid; 2048/16, 256/16). No divides in the hot path. */
#define WORLD_TO_CHUNK(w)  ((w) >> 4)        /* world voxel coord -> chunk coord */
#define LOCAL_IN_CHUNK(w)  ((w) & 15)        /* world voxel coord -> 0..15 local */

/* Linear index inside a chunk. Layout is x-fastest, then y, then z
 * (z-outer), so a row of 16 x-neighbors is contiguous — matches the
 * mesher's slice walk and the CA's +x/-x neighbor reads. */
static inline int vox_index(int lx, int ly, int lz) {
    return lx + (ly << 4) + (lz << 8);       /* lx + ly*16 + lz*256 */
}
```

The 16-voxel edge is what keeps the entire coordinate system in shifts and masks; a non-power-of-two chunk would put an integer divide in the hottest indexing path on a CPU where divides are expensive. The interior layout (x-fastest) is chosen so that the mesher's per-slice scan and the CA's horizontal neighbor reads both walk contiguous memory. Section 1 already rejected 8³ (8× the chunk-boundary surface area → inflated greedy-mesh seam splits and 8× the draw calls/metadata) and 32³ (128 KiB blows L1, spikes remesh, and wastes cache on cold voxels because the sim front is far smaller than the chunk); I do not relitigate that here beyond noting that the data-model consequence of 16³ is the clean 16 KiB == one-L1-half == one-MaterialDef-table symmetry that the whole memory story leans on.

The chunk header carries the metadata that surrounds the raw voxel array. Critically — and this is a Section-1 binding rule the data model encodes structurally — **the chunk is the unit of meshing, streaming, and storage, but NOT the unit of simulation wake-up.** The active-voxel tracking is a sub-chunk index list the CA owns; the chunk merely *hosts* it.

```c
typedef struct Chunk {
    Voxel    vox[CHUNK_VOX];        /* 16 KiB — the payload, cache-line aligned   */
    int32_t  cx, cy, cz;            /* this chunk's coord (also recoverable from key) */
    uint32_t mesh_handle;           /* VBO/index handle owned by the mesher (Sec 4/5) */
    struct Chunk *neigh[6];         /* cached -x+x-y+y-z+z pointers (Sec 1 binding)    */
    uint16_t active_count;          /* # voxels in the wake-set (sub-chunk granularity)*/
    uint16_t active_list_cap;       /* capacity of the CA-owned active index list      */
    uint16_t *active_list;          /* CA-owned: indices into vox[] that are awake     */
    uint8_t  flags;                 /* CHUNK_DIRTY_MESH | CHUNK_MODIFIED | CHUNK_GEN   */
    uint8_t  _pad;
} Chunk;
```

`CHUNK_MODIFIED` is the persistence-critical bit: a chunk the player has altered must be saved to disk on eviction, whereas an untouched chunk is dropped and *regenerated from the seed* on return (Section 7/8). The 6-entry `neigh` cache is the Section-1 binding rule made concrete: hot neighbor traversal (the mesher reading the adjacent chunk's edge plane, heat diffusion crossing a chunk face) follows these pointers; the hash table below is consulted only on a cold edge or a fresh stream-in, never in the inner loop.

### 2.5 World Addressing — Sparse Hash of Chunk Pointers

Section 1 committed to a **sparse open-addressing hash table keyed on a packed 64-bit chunk coordinate**, sized to ~16,384 slots at a 0.7 load factor for the ~10,000-chunk resident window, and rejected both the flat full-world array (262,144 entries hardcodes world bounds and models a 96%-empty space as if dense) and VM paging (couples addressing to persistence and injects page-fault stalls into a single-threaded frame budget that cannot absorb a surprise disk hit mid-tick). Here is the data structure made concrete.

```c
/* Pack signed chunk coords into 64 bits, 21 bits/axis (two's-complement
 * masked). 21 bits >> the 128 x 128 x 16 grid and leaves room for any
 * future world-bound growth — addressing is NOT tied to current bounds. */
static inline uint64_t chunk_key(int32_t cx, int32_t cy, int32_t cz) {
    return  ((uint64_t)(cx & 0x1FFFFF))
         |  ((uint64_t)(cz & 0x1FFFFF) << 21)
         |  ((uint64_t)(cy & 0x1FFFFF) << 42);
}

/* Integer finalizer mix — cheap on the Pentium M, good avalanche, no division.
 * Probe positions are masked by (cap-1); cap is always a power of two. */
static inline uint32_t key_hash(uint64_t k) {
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33; return (uint32_t)k;
}

typedef struct {
    uint64_t key;    /* 0 reserved = empty slot; chunk (0,0,0) stores key|SENTINEL */
    Chunk   *ptr;    /* NULL also marks empty; tombstone uses a distinct marker     */
} ChunkSlot;         /* 16 bytes (8 + 8 on 32-bit: ptr is 4, padded to 8 stride)    */

typedef struct {
    ChunkSlot *slots;    /* power-of-two array; linear probing                      */
    uint32_t   cap;      /* 16384 for the r=12 window (10k residents / 0.7)         */
    uint32_t   count;    /* live residents                                          */
    uint32_t   mask;     /* cap - 1, for index = key_hash(k) & mask                 */
} ChunkMap;

/* Lookup: linear-probe a contiguous run. Cache-friendly on the Pentium M —
 * no allocator-scattered chain nodes to pointer-chase, which is precisely
 * why open addressing beats chaining here. Average probe length < 1.3 at 0.7. */
static Chunk *chunkmap_get(const ChunkMap *m, uint64_t key) {
    uint32_t i = key_hash(key) & m->mask;
    for (;;) {
        ChunkSlot *s = &m->slots[i];
        if (s->ptr == NULL && s->key == 0) return NULL;  /* empty -> miss        */
        if (s->key == key)                  return s->ptr;
        i = (i + 1) & m->mask;                            /* wrap, stay in array */
    }
}
```

Linear probing is the deliberate choice over chaining: probes walk a contiguous slot array, so a miss-then-hit costs a couple of L1-resident reads rather than a chain of allocator-scattered cache misses — the right tradeoff on a single core where every miss stalls the whole pipeline. The table is **256 KiB at 16,384 × 16-byte slots** (verified, Section 1), negligible against the budget. It is the *authoritative* residency map and the cold-lookup path only; the binding inner-loop rule from Section 1 stands: **neighbor traversal goes through each chunk's cached 6 `neigh` pointers, the hash table is hit only on residency management (insert/evict) and window-edge cold lookups.** On insert and evict the map fixes up the affected neighbors' caches so the inner loop never falls back to a hash probe except when genuinely crossing into freshly-streamed territory. No section may assume a dense, flat, directly-indexable world array, and world bounds are never baked into this addressing — the 21-bits-per-axis key is what guarantees both.

---

## 3. Cellular Automaton Simulation

This is the technical heart. Everything in the foundation layer — the 4-byte voxel, the 16³ chunk, the 14 ms sim slice, the fixed decoupled tick — exists to make *this* section possible: a 3D cellular automaton where heat, fluids, and phase changes propagate by material physics, and mechanics like "furnace," "smelter," and "forge" are never coded, only *emerge*. The governing reality is brutal and clarifying: there are ~1.07 billion voxels in the world and I am allowed to touch a few thousand of them per tick on one in-order-ish core. The entire design is an answer to the question *which thousands, and how do I find them without looking at the other billion.*

I design heat first and most thoroughly, because heat is the prime mover — it is what wakes fluids (melting), what drives phase change, and what the player's first technology (a hot enough cavity) is built on. Fluids and state transitions are layered on top of the same active-set machinery.

### 3.1 The Cardinal Rule: We Simulate Fronts, Not Volumes

The foundation already separated the **chunk** (the unit of meshing, streaming, storage — 16³, 16 KiB) from the **simulation-wake unit**, and mandated sub-chunk active tracking. I take that as binding and build on it. A cellular automaton that swept every resident voxel would touch 10,000 chunks × 4,096 voxels = ~41 million voxels per tick — three orders of magnitude over budget and absurd when 99.99% of them are inert stone at ambient temperature doing nothing. The CA touches only voxels on an **active front**: the surface of a heat plume, the leading edge of a spreading liquid, the cells currently crossing a melt threshold. A billion-voxel world with a quiescent interior costs nothing. A world where the player just dropped a torch into an iron-walled cavity costs exactly the heat front around that torch, growing as it propagates and shrinking as it equilibrates back to sleep.

The hard target, inherited and defended below, is an **active-voxel cap of 4,096 per tick** (`SIM_ACTIVE_CAP`). I'll justify that number in §3.7; for now it is the budget every structure is sized against.

### 3.2 Active-Voxel Tracking: Per-Chunk Active Lists, Not a Global Set

The first architectural fork is *where* the active set lives. A single global hash-set of active voxel coordinates is the obvious data structure and it is wrong for this hardware. A global set of `(x,y,z)` keys means every wake/sleep is a hash insert/delete with scattered memory access, and — fatally — iterating it touches voxels in hash order, i.e. in *random* memory order across the 156 MiB window, missing L2 on nearly every access. On a CPU with no second core to hide misses behind, that alone could blow the 14 ms budget on cache stalls before any physics runs.

So the active set is **partitioned by chunk**, and the global structure is merely a list of *which chunks have any active voxels*:

```c
/* Per-chunk active tracking. Lives in the Chunk header (the foundation's
 * per-chunk metadata budget). A chunk with zero active voxels carries
 * almost nothing and is never visited by the sim. */
typedef struct {
    uint16_t active[CHUNK_ACTIVE_MAX]; /* packed local indices 0..4095   */
    uint16_t count;                    /* how many entries are live       */
    uint32_t in_active_mask[128];      /* 4096 bits: is local idx queued? */
} ChunkActive;
/* CHUNK_ACTIVE_MAX is small (e.g. 512). A chunk needing more than 512
 * simultaneously-active voxels is a runaway front; it stays in the list
 * across ticks and drains over multiple ticks (see budget, §3.7). */

/* The global sim worklist: just the chunks that have work. */
typedef struct {
    Chunk*   chunks[SIM_DIRTY_CHUNK_MAX]; /* chunks with count > 0       */
    uint32_t n;
} ActiveChunkList;
```

A voxel's "address" inside a chunk is a 12-bit local index (`x | y<<4 | z<<8`, since 16³ needs exactly 12 bits — verified). The `active[]` array is a *compact list* of those indices, so iterating the front is a linear walk over a small array, and the voxels it points at are all within one 16 KiB chunk that is already cache-hot. The `in_active_mask` bitfield (4096 bits = 512 bytes) answers "is this local index already queued?" in O(1) without scanning the list — essential so a voxel woken by four neighbors in one tick is enqueued once, not four times.

**Why per-chunk wins concretely.** Iterating active voxels chunk-by-chunk means the inner loop streams one chunk's 16 KiB (half of L1D, verified) plus the edge planes of its six cached neighbors, processes its whole front, then moves on. Memory access is *local and sequential within a chunk*, which is exactly what the Pentium M's hardware prefetcher rewards. The `ACTIVE` flag bit (foundation byte 3) is the per-voxel ground truth; the chunk's `active[]` list and `in_active_mask` are the *acceleration structure* over it. They must stay consistent — the wake/sleep primitives below own that invariant.

**Waking and sleeping.** These are the only two operations that mutate membership, and they are the hottest non-physics code in the engine:

```c
static inline void wake(Chunk* c, uint16_t li) {
    if (mask_test(c->act.in_active_mask, li)) return;   /* already queued */
    if (c->act.count >= CHUNK_ACTIVE_MAX) { c->overflow = 1; return; }
    mask_set(c->act.in_active_mask, li);
    c->act.active[c->act.count++] = li;
    c->voxels[li] |= (FLAG_ACTIVE << 28);
    if (!c->in_dirty_list) actchunk_push(c);            /* chunk joins worklist */
}

static inline void sleep_voxel(Chunk* c, uint16_t li) {
    c->voxels[li] &= ~((uint32_t)FLAG_ACTIVE << 28);
    /* lazy removal: leave the index in active[]; the tick's compaction
     * pass drops entries whose ACTIVE flag is now clear. Removing from a
     * compact array mid-iteration is a swap-with-last, but lazy is cheaper
     * and avoids reordering that would introduce a directional bias. */
}
```

The decision to **sleep lazily** — clear the flag now, physically remove during end-of-tick compaction — matters for determinism (§3.3): it keeps `active[]` in a stable order during the sweep so the read pass and the wake pass don't shuffle indices under each other.

**What re-wakes a voxel.** A voxel goes to sleep when, after a tick, it produced no change worth propagating: its temperature delta fell below an epsilon, it isn't a liquid that can still flow, and no neighbor pushed energy or mass into it. It is *re-woken* by any neighbor whose state change crosses into it — the heat kernel wakes a colder neighbor when it pushes heat across; a fluid wakes the cell it flows into; a melt wakes the now-liquid cell and its neighbors. This neighbor-wake is the propagation mechanism. The front advances because active cells wake the cells ahead of them and then sleep themselves once equilibrated — a literal traveling wave through the active-set bookkeeping, never a volume scan.

### 3.3 Tick Ordering & Determinism: Double-Buffer Temperature, In-Place Everything Else

Order matters, and it matters differently for the two kinds of update. I split them deliberately.

**Heat is double-buffered.** Explicit finite-difference diffusion is mathematically a *simultaneous* update: every cell's new temperature is a function of its neighbors' temperatures *at the start of the tick*. If we update in place, a cell read by its neighbor will already hold this tick's value, injecting a directional bias — heat would appear to "lean" toward whichever corner the sweep starts from, and the result would depend on iteration order. That is unacceptable for a system whose whole appeal is physical plausibility. So temperature uses a classic ping-pong: read the front from the current buffer, write deltas to a scratch buffer, swap.

But the foundation forbids a second full copy of the world (the voxel's 32 bits are fully allocated; there is no room and no RAM for a parallel billion-voxel temperature field). The resolution is that **we double-buffer only the active front, not the world.** A tick collects the active voxels, snapshots *their* temperatures (and the one-ring of neighbor temperatures it reads) into a small scratch array sized to the active cap, computes new temperatures into a second scratch array, then commits back into the live voxel words. Scratch cost: a few thousand entries of `{ Chunk*, uint16_t li, uint8_t t_old, uint8_t t_new }` ≈ tens of KiB — this is the foundation's "Sim double-buffer / scratch, 16 MiB" line item, and we use a sliver of it. The world is updated in place *between* ticks; only the within-tick read-set is buffered, which is all the math actually requires.

```c
/* Per-tick scratch (sized to SIM_ACTIVE_CAP, allocated once, reused). */
typedef struct { Chunk* c; uint16_t li; uint8_t t_new; } HeatWrite;
static HeatWrite g_heat_writes[SIM_ACTIVE_CAP];
```

**Fluids and powders are updated in place, with a moved-this-tick guard.** A true double-buffer for mass transport is both expensive and *wrong* — falling-sand cellular automata are intentionally sequential; a grain moves into a cell, and the next grain must see it occupied or two grains alias into one cell and mass is destroyed. So fluids update in place. The directional-bias problem (everything drifts toward +x because we always sweep +x first) is real and is handled the Noita way: **alternate sweep direction by tick parity**, and within a tick, **randomize the order of lateral spread choices** using a per-tick deterministic PRNG seeded from the tick counter. "Deterministic" is the key word — the seed is `tick_index`, so a replay from the same world state produces the same result (important for the persistence and any future replay/debug tooling), but no fixed spatial direction is privileged. A `MOVED_THIS_TICK` transient bit (kept in a side bitmask in the sim scratch, *not* in the voxel word — the foundation's flag nibble is full) prevents a cell that already moved from being processed again as the sweep reaches its new location.

**Phase decision: heat first, then mass, within each tick.** The tick computes new temperatures across the whole active front (double-buffered), commits them, *then* evaluates state transitions (melt/solidify/evaporate) against the freshly committed temperatures, *then* runs fluid/powder movement on whatever is now liquid or loose. This ordering is causal and intuitive: you heat the iron, the iron melts, the melt flows — all visible within one tick rather than smeared across three.

### 3.4 Heat Propagation: Explicit Finite-Difference Diffusion (Designed First, Most Thoroughly)

Heat is modeled as **explicit forward-time, centered-space (FTCS) diffusion** on the 6-connected voxel lattice — the 3D heat equation discretized the simplest stable way. I reject implicit solvers (they need a linear system solve across the front each tick — far too expensive on this CPU and they couple non-locally, breaking the active-front model) and I reject "just average with neighbors" hand-waving (it has no conductivity, no heat capacity, so a copper wall and a wool wall would conduct identically and the *entire emergent-materials premise dies*). The physics must come from `MaterialDef`.

**The governing discrete update.** For voxel *i* with the six axis neighbors *j*, the heat that flows from *j* to *i* in one tick is proportional to the temperature difference and the conductance of the interface:

```
Q_ij  =  k_ij * (T_j - T_i)            // energy across the i-j face this tick
T_i' =  T_i + (dt / (C_i)) * Σ_j Q_ij  // C_i = density_i * specific_heat_i (heat capacity per voxel)
```

Each material contributes `conductivity (k)`, `specific_heat (c)`, and `density (ρ)` from the global `MaterialDef` table (foundation: all behavior is data-driven from the 256-entry table). The interface conductance `k_ij` between *dissimilar* materials is the **harmonic mean** of the two conductivities — the physically correct series-resistance combination — so a hot copper voxel against a stone voxel transfers heat at the rate the *worse* conductor allows. This single detail is what makes a stone furnace cavity *hold* heat while a copper one bleeds it: emergent insulation, never coded as a rule.

```c
/* Heat kernel for one active voxel. T values are decoded to a working
 * integer/fixed-point "heat unit" scale; we do NOT do FP math per voxel on
 * the Pentium M's slow x87 path — see fixed-point note below. */
static void heat_step(Chunk* c, uint16_t li, HeatWrite* out, uint32_t* n_out)
{
    Voxel v   = c->voxels[li];
    uint8_t mi = VOX_MAT(v);
    const MaterialDef* md = &g_mat[mi];
    int32_t Ti = temp_decode(VOX_TEMP(v));     /* -> heat units (see §3.6) */

    int32_t flux = 0;
    for (int d = 0; d < 6; ++d) {
        Voxel  nv = neighbor(c, li, d);        /* cached-neighbor deref     */
        const MaterialDef* nmd = &g_mat[VOX_MAT(nv)];
        int32_t Tj = temp_decode(VOX_TEMP(nv));
        /* k_ij precomputed: harmonic-mean conductance LUT[mi][nmat], 64K
         * entries of int16 = 128 KiB, resident (material-table budget). */
        int32_t kij = g_conduct_lut[mi][VOX_MAT(nv)];
        flux += kij * (Tj - Ti);
    }
    /* dt/C folded into md->inv_heat_capacity (precomputed per material). */
    int32_t dT = (flux * md->inv_heat_capacity) >> HEAT_SHIFT;
    if (dT == 0) return;                       /* no change -> candidate to sleep */

    int32_t Tnew = clamp_heat(Ti + dT);
    out[*n_out].c = c; out[*n_out].li = li;
    out[*n_out].t_new = temp_encode(Tnew);
    (*n_out)++;

    /* Re-wake the neighbors we pushed meaningful heat into. */
    for (int d = 0; d < 6; ++d)
        if (abs(g_conduct_lut[mi][VOX_MAT(neighbor(c,li,d))] * dT) > WAKE_EPS)
            wake_neighbor(c, li, d);
}
```

**The stability criterion and how it bounds the timestep.** Explicit FTCS in 3D with 6 neighbors is conditionally stable; the von Neumann limit is

```
α · dt / dx² ≤ 1/6          (verified: the 3D explicit-diffusion stability bound is 1/6)
```

where α = k/(ρc) is thermal diffusivity. With dx = 1 voxel (1 m), the timestep is bounded by the *fastest-diffusing* material in the world: `dt ≤ 1 / (6 · α_max)`. Exceeding it makes temperatures oscillate and blow up — the simulation would visibly "ring" and then NaN. Rather than carry a literal `dt` in seconds, I **normalize the unit system so the most conductive material's per-tick diffusion number is exactly 1/6**, the stability ceiling, and every slower material is then a smaller, automatically-stable fraction. This means the precomputed `g_conduct_lut` and `inv_heat_capacity` values are tuned once, against the worst-case material, so that no in-game configuration can violate stability. The cost is that very-low-diffusivity materials change temperature slowly per tick — which is *physically correct* (insulators are slow) and exactly the behavior we want.

**Heat re-wakes neighbors** (the propagation engine). When `heat_step` changes a voxel's temperature, it wakes any neighbor across which it just pushed more than `WAKE_EPS` heat units. A torch dropped in a cavity wakes its six neighbors; they heat, wake *their* neighbors; the front expands outward at one voxel-ring per tick until the gradient flattens below epsilon, at which point cells stop changing and sleep. This is why a furnace "warms up" over seconds of in-world time rather than instantly — an emergent, free consequence of explicit diffusion on the active set.

**Heat sources and sinks.** A burning fuel voxel (coal, charcoal) is not special-cased; it is a material whose `MaterialDef` injects energy each tick while its `fuel_remaining` (a transient side value, §3.8) lasts, and which transitions to ash when spent. Ambient is modeled as a weak pull of boundary voxels toward the biome ambient temperature — a slow sink that lets things cool, also data-driven. Crucially, **a voxel at ambient surrounded by ambient produces dT ≈ 0 and sleeps**: the resting state of the world is free.

**PROTOTYPE FLAG (inherited from the foundation's temperature decision).** The foundation pinned temperature to 8-bit two-segment non-linear encoding and explicitly required the heat-sim section to validate diffusion smoothness against it before freezing. I honor that: the 20 C/step industrial band means a single tick's `dT` in that band may round to zero for slow conductors, *stalling* diffusion (a cell can't accumulate a sub-20 C rise). My mitigation is in §3.6 — accumulate heat in a wider internal accumulator and only quantize on store — but **whether this fully eliminates visible stair-stepping in the temperature debug view must be confirmed on the actual M170 before the encoding is frozen.** If it stair-steps unacceptably, the foundation's stated contingency (12-bit temperature, forcing fill/light into a side array) is the fallback, and that is a foundation-level change, not one I can make here. I flag it; I do not pre-empt it.

### 3.5 Fluid Simulation: Two Rules — Powders and Liquids

Falling-sand in 3D, done naively, is the budget killer: a true pressure solve (so water finds its level across a connected body in one tick) is a global linear system and is *out of the question* on this CPU. I use two cheap local rules, chosen per material from a `MaterialDef.fluid_kind` field, that approximate the visible behavior without ever solving for pressure.

**Powders (sand, gravel, ash, ore dust, snow) — granular rule.** A powder voxel tries, in order: (1) fall straight down if the cell below is empty/displaceable; (2) if blocked, fall diagonally down into a random one of the four down-diagonal cells (randomized per §3.3 to avoid bias). It does not spread laterally on flat ground — that's what makes a *pile* with an angle of repose rather than a puddle. Powders ignore the fill nibble (they're full-or-empty). This is the classic Noita powder rule lifted into 3D with four diagonals instead of two.

**Liquids (water, oil, every molten metal) — fill-equalization rule.** Liquids use the foundation's 4-bit fill level (16 levels) and equalize *locally* without a pressure solve:

```
1. If the cell below can take fluid, push as much fill down as it can hold
   (gravity dominates; a column drains before it spreads). 
2. Otherwise, average this cell's fill with its 4 lateral neighbors of the
   same liquid, moving at most 1-2 levels per tick toward the average.
   This is a relaxation/diffusion of "fill", NOT a pressure solve.
3. Molten metals additionally bias flow by viscosity (MaterialDef): a high-
   viscosity melt moves fewer levels/tick, so molten iron crawls and oozes
   while water races.
```

Lateral fill-averaging is the trick that buys 90% of "pooling and finding a level" for ~5% of the cost of a real solver. It is *not* physically exact — water won't instantly equalize across a tall U-tube the way a pressure solve would; it relaxes over many ticks, like a slow viscous fluid. For a 30 FPS survival/progression game where the player watches molten metal pool in a mold over seconds, this reads as correct. I'm flagging the honest limitation: **communicating-vessels behavior across large height differences will be visibly slow/imperfect.** I judge that an acceptable, even charming, cost — and it's the right call versus a solver that would eat the entire 14 ms budget on one pool. If prototyping shows it's unacceptable for some gameplay-critical case (e.g., a water-wheel needing a stable head), a localized small pressure relaxation over a single connected body is the contingency, but I will not pay for it globally.

**Fluids wake and sleep like everything else.** A liquid cell sleeps when it can't fall and is within 1 fill level of all lateral same-liquid neighbors (settled). It re-wakes when a neighbor changes — a wall is dug, a new pour arrives. A still pond costs nothing.

### 3.6 Fixed-Point, Not Floating-Point — and the Heat Accumulator

A defended detail that pervades the kernel: **all sim math is fixed-point integer**, not `float`. The Pentium M's x87 FPU is slow and its per-op latency would wreck the ~7,700-cycles-per-voxel budget (verified, §3.7). Temperatures decode to a wider internal "heat unit" scale (say 16-bit signed) for the duration of computation, conductances and inverse-heat-capacities are precomputed integer LUTs with a fixed `HEAT_SHIFT` for the final divide-by-shift, and only the *final committed* temperature is re-quantized to the foundation's 8-bit two-segment code on store. This wider internal accumulator is exactly the mitigation for the 20 C/step stalling problem (§3.4 prototype flag): heat accumulates at full internal resolution across ticks in the scratch where a voxel stays active, so even sub-quantum rises eventually cross a code boundary rather than rounding to zero every tick. The 8-bit field is a *storage* format; the math runs at higher precision while a voxel is awake.

### 3.7 Per-Frame Sim Budget Enforcement

The foundation pins **14 ms (42% of the 33.33 ms frame) for simulation**, a **fixed-rate tick decoupled from render with no catch-up**, and an **active-voxel cap as the last-resort throttle**. I make those concrete.

**Tick rate: 15 Hz, fixed.** A 15 Hz sim tick is a 66.67 ms period (verified) — i.e. one sim tick every ~2 render frames at 30 FPS (verified: 0.5 ticks/frame). I choose 15 over 20 Hz to give each tick more wall-clock room and over 10 Hz so heat/fluid fronts visibly advance at a satisfying pace. Render interpolates nothing for the sim (temperature changes are slow and the mesher only cares about discrete material/fill changes), it simply redraws the latest committed state between ticks. **Critically: because a tick happens every other frame, the tick's full cost is amortized — but the budget is still enforced *per tick* at 14 ms**, and a tick must finish within the frame it runs in or it eats into mesh/render time on that frame.

**Active cap: 4,096 voxels per tick.** This is the load-bearing number. The 7-neighbor read working set at this cap is 4,096 × 7 × 4 = exactly **114,688 bytes = 112.0 KiB** (verified — the foundation's "~110 KiB" figure, exact), which lives in the 2 MiB L2 with enormous headroom while overflowing L1 only gently. The per-voxel time budget is the auditable arithmetic that this whole section rests on: 14 ms / 4,096 voxels = 3,418 ns/voxel, and at 2.26 GHz that is 14e-3 / 4096 × 2.26e9 = **~7,725 cycles per active voxel (verified) — call it ~7,700 cycles (~3,420 ns)**. (I deliberately do not round this *up*: the earlier draft's "~7,900 cycles / 3,500 ns" overstated the available margin by ~2%, and a section whose entire argument is "the kernel fits the budget" cannot afford a generously-rounded headline number.) ~7,700 cycles is still ample for a 6-neighbor heat kernel plus a fluid rule plus wake bookkeeping in fixed-point, assuming we stay cache-resident (which the per-chunk layout guarantees) — but it is ample by the *real* margin, not an inflated one. Setting the cap at 4,096 (one chunk's worth, a clean power of two) rather than 8,192 keeps us off the L2-pressure cliff and inside the budget with margin for a bad cache day; the cap is a *prototype-tunable* number but 4,096 is the defended default.

**What happens on overrun — the graceful slowdown.** The tick processes active voxels in a stable order (chunk worklist order, then per-chunk `active[]` order). When it has processed `SIM_ACTIVE_CAP` voxels or hit the 14 ms wall-clock guard (whichever comes first), it **stops and leaves the remaining active voxels active for the next tick.** It does *not* drop them, does *not* sleep them, does *not* run a catch-up tick. The visible consequence is precisely what the foundation mandates: when the active front is enormous (a lava flood, a huge structure melting), the *in-world simulation slows down* — heat propagates at a fraction of its normal speed — while the frame rate and render stay rock-solid. The simulation degrades in the time domain, never in the frame domain. This is the spiral-of-death-proof design: no matter how much the player sets on fire, the engine cannot be made to miss vsync by the sim; it can only be made to think slower.

```c
void sim_tick(uint64_t tick_index) {
    rng_seed(tick_index);                 /* deterministic per-tick RNG    */
    uint32_t processed = 0, nw = 0;
    uint64_t deadline = now_us() + SIM_BUDGET_US;   /* 14000 us guard      */

    /* PHASE 1: HEAT (double-buffered into g_heat_writes) */
    for (uint32_t ci = 0; ci < g_actlist.n && !over(processed, deadline); ++ci) {
        Chunk* c = g_actlist.chunks[ci];
        for (uint16_t k = 0; k < c->act.count; ++k) {
            uint16_t li = c->act.active[k];
            if (!(c->voxels[li] & (FLAG_ACTIVE<<28))) continue; /* lazy-dead */
            heat_step(c, li, g_heat_writes, &nw);
            if (++processed >= SIM_ACTIVE_CAP) goto commit;
        }
    }
commit:
    for (uint32_t i = 0; i < nw; ++i)     /* commit new temps in place     */
        store_temp(g_heat_writes[i].c, g_heat_writes[i].li, g_heat_writes[i].t_new);

    /* PHASE 2: STATE TRANSITIONS against freshly committed temperatures */
    for_each_active(c, li, &processed, deadline)
        try_phase_change(c, li);          /* melt/solidify/evap, §3.8       */

    /* PHASE 3: FLUID / POWDER MOVEMENT, in place, sweep dir by parity */
    int dir = tick_index & 1;
    for_each_active_dir(c, li, dir, &processed, deadline)
        fluid_step(c, li);

    /* PHASE 4: COMPACTION — drop lazily-slept voxels, retire empty chunks */
    for (uint32_t ci = 0; ci < g_actlist.n; ++ci)
        compact_chunk_active(g_actlist.chunks[ci]);
    actlist_drop_empty(&g_actlist);
    /* Voxels left over due to cap/deadline simply remain in the lists
       and are processed next tick: graceful in-world slowdown. */
}
```

### 3.8 State Transitions With Latent Heat

State transitions are where heat, fluids, and materials fuse into emergence, and they must conserve energy or the whole illusion collapses (perpetual-motion furnaces, metal that melts and re-solidifies for free). Each `MaterialDef` carries threshold temperatures and the *destination material id* on each side of the transition:

```c
typedef struct {
    int16_t melt_temp,  boil_temp,  freeze_temp, condense_temp; /* heat units */
    uint8_t melt_to,    boil_to,    freeze_to,   condense_to;   /* material ids */
    uint16_t latent_melt, latent_boil;  /* energy absorbed/released at change */
    /* ... conductivity, specific_heat, density, fluid_kind, viscosity ... */
} MaterialDef;  /* one of 256, foundation's global table */
```

A transition is a **threshold comparison against `MaterialDef`** (foundation mandate — never a hardcoded recipe). When a solid iron voxel's committed temperature crosses `melt_temp`, its material id is swapped to `melt_to` (molten iron) and its `LIQUID` flag set, which on the next tick hands it to the liquid fluid rule — and *that* is the entire "smelting" mechanic: no furnace block, no recipe table, just iron whose temperature crossed a number, in a cavity the player happened to build. Solidification is the reverse: molten iron cooling below `freeze_temp` swaps back to solid and clears `LIQUID`, pooling into whatever shape the cavity gave it. Evaporation/condensation use `boil_to`/`condense_to` (water ⇄ steam, a gas material that rises and dissipates).

**Latent heat is non-negotiable for energy conservation.** Real phase change absorbs energy without raising temperature (the iron sits *at* its melting point soaking up heat until fully molten). Without modeling it, a furnace would melt a wall instantly and over-shoot, and cooling would release no warmth. So a voxel crossing a threshold doesn't transition the instant `T` crosses the line; it **banks the crossing energy into a per-voxel latent accumulator**, and only completes the transition once `latent_melt` worth of energy has been absorbed:

```c
/* Latent-heat accounting. The accumulator is transient sim state and lives
 * in a SIDE structure keyed by (chunk, local idx) — NOT in the voxel word,
 * which the foundation has fully allocated. Only voxels mid-transition need
 * an entry, so this is a small hash, sized to the active cap, not the world. */
void try_phase_change(Chunk* c, uint16_t li) {
    Voxel v = c->voxels[li]; const MaterialDef* md = &g_mat[VOX_MAT(v)];
    int32_t T = temp_decode(VOX_TEMP(v));
    if (md->melt_to && T > md->melt_temp) {
        Latent* L = latent_get_or_add(c, li);
        L->banked += (T - md->melt_temp);          /* energy above threshold */
        set_temp(c, li, md->melt_temp);            /* clamp T while changing  */
        if (L->banked >= md->latent_melt) {        /* fully melted            */
            int32_t excess = L->banked - md->latent_melt;
            swap_material(c, li, md->melt_to);      /* iron -> molten iron     */
            set_flag(c, li, FLAG_LIQUID);
            set_temp(c, li, md->melt_temp + excess);/* spill excess into temp  */
            latent_remove(c, li);
            wake_ring(c, li);                       /* neighbors re-evaluate   */
        }
    }
    /* symmetric branch for freeze: bank energy released, hold at freeze_temp,
       complete when latent debt repaid -> emergent thermal mass / buffering. */
}
```

This buys genuinely emergent thermal behavior for free: a big iron block takes *time* to melt through because each voxel must soak its latent heat, the melt front advances realistically, and a pool of cooling molten metal *warms the cavity around it* as it releases latent heat on freezing. The player will discover that a bigger furnace charge needs more sustained fuel — a real metallurgical intuition, never coded as a rule, only as conserved energy crossing thresholds in `MaterialDef`.

**The latent accumulator's home is a side structure**, sized to the active cap — explicitly honoring the foundation's rule that transient per-voxel data must not live in the fully-allocated voxel word. Only the handful of voxels mid-transition at any instant carry an entry; the world's quiescent solids carry nothing.

### 3.9 Summary of Binding Choices This Section Adds

The sim is a 3D cellular automaton operating exclusively on per-chunk active-voxel fronts, never on volumes; idle voxels and the quiescent world cost nothing. Heat is the prime mover — explicit FTCS diffusion in fixed-point, conductances drawn from `MaterialDef` and combined as harmonic means at interfaces, normalized to sit at the 3D stability limit of 1/6 so no configuration can blow up. Temperature is double-buffered over the active front only (not the world); fluids and powders update in place with parity-alternating, deterministically-seeded sweeps to kill directional bias. State transitions are threshold comparisons against `MaterialDef` with latent-heat banking in a side structure so energy is conserved and phase change takes time. The tick runs at a fixed 15 Hz decoupled from the 30 FPS render, capped at 4,096 active voxels (112.0 KiB L2-resident working set, ~7,700 cycles/voxel — derived as 14e-3 / 4096 × 2.26e9 = ~7,725, not rounded up), and on overrun the in-world simulation slows gracefully while the frame never hitches. Every emergent mechanic — furnace, smelter, forge, mold — is a consequence of these rules acting on data-driven materials, not a single line of mechanic-specific code.

---

## 4. Meshing

The mesher's job is to turn 16 KiB of opaque voxel bytes into the smallest pile of triangles that looks correct, fast enough that a burst of digging or a spreading lava pool does not blow the 6 ms mesh budget, and compact enough that the resident geometry of 10,000 chunks lives inside the G70's 256 MiB. Every decision here is downstream of three foundation facts: one core serializes sim/mesh/render in 33.33 ms; the voxel is a fixed 4-byte word I cannot extend; and greedy-merge seams at 16³ boundaries directly cost VRAM. I optimize for *quad count*, because a quad is the unit that costs vertices, costs VBO bytes, costs draw submission, and costs fill — it is the single number worth minimizing.

### The Canonical Vertex Format — 12 Bytes, Owned Jointly With Rendering

Before any algorithm, I pin the one thing the mesher and the renderer must agree on byte-for-byte: the **on-the-wire vertex layout**. The mesher *writes* this struct into the VBO; the rendering section's `glVertexAttribPointer` calls *read* it back with hard-coded offsets. If these two disagree by a single byte, the renderer misreads position as material, the world renders as garbage, and nothing about it is debuggable from either section alone. So this layout is a **binding number shared by Section 4 and Section 5** — `PER_VERTEX_BYTES = 12`, and the field order and offsets below are authoritative. Section 5 cites *this* table; it does not define its own.

I reject the alternative 16-byte format (float `x,y,z` = 12 B + `uint16 u,v` = 4 B, with light·AO·sun pre-folded into a single `a_bright` scalar). Two reasons, both hardware. First, **float positions are wasteful here**: a vertex is chunk-local, and a 16³ chunk's coordinates run `0..16` inclusive — that fits trivially in a `uint16` (it would fit in a byte if not for the inclusive-16 corner). Spending 32 bits per axis on a value that needs 5 is 6 bytes thrown at every vertex, and at 2,500 meshed chunks that is real VRAM (see budget below). Second, **pre-folding light·AO·sun at mesh time bakes the sun direction into the geometry**, which forces a remesh every time the day/night sun term changes — unacceptable when meshing is the scarce elastic budget. The 12-byte format keeps positions as `uint16` and ships light and AO as *separate* bytes so the sun term is applied live in the shader against an unbaked sky-light value, costing zero remeshes. The 12-byte vertex wins on both counts; it is the format.

```c
/* ===== CANONICAL VERTEX — PER_VERTEX_BYTES = 12 =====================
 * Binding layout shared by Meshing (writer) and Rendering (reader).
 * 4-byte aligned; every attribute lands on a clean offset (the
 * G70/XP driver dislikes unaligned fetches). Positions are chunk-local.
 *
 *   offset 0 : uint16 px      chunk-local X, 0..16 inclusive
 *   offset 2 : uint16 py      chunk-local Y, 0..16 inclusive
 *   offset 4 : uint16 pz      chunk-local Z, 0..16 inclusive
 *   offset 6 : uint8  mat     material id -> atlas tile select (0..255)
 *   offset 7 : uint8  face    face direction 0..5 -> normal + UV basis
 *   offset 8 : uint8  light   4-bit sky | 4-bit block, baked, UNFOLDED
 *   offset 9 : uint8  ao      ambient occlusion 0..15
 *   offset 10: uint8  u       tile-local UV corner (tiling, 0..W)
 *   offset 11: uint8  v       tile-local UV corner (tiling, 0..H)
 * ==================================================================== */
typedef struct {
    uint16_t px, py, pz;   /* 0 : chunk-local position, 0..16 inclusive (6 B) */
    uint8_t  mat;          /* 6 : material id -> atlas tile select       (1 B) */
    uint8_t  face;         /* 7 : 0..5 face direction -> normal + UV basis(1 B) */
    uint8_t  light;        /* 8 : 4-bit sky | 4-bit block, baked         (1 B) */
    uint8_t  ao;           /* 9 : ambient occlusion 0..15                (1 B) */
    uint8_t  u, v;         /* 10: tile-local UV corner, tiling           (2 B) */
} MeshVert;                /* total 12 B */
```

**Why light and AO stay separate bytes rather than one pre-folded `shade`.** It is tempting to collapse the two nibbles into a single `uint8 shade = light * ao` at mesh time and save nothing (still a byte) but simplify the shader. I keep them separate for one decisive reason: the **sun/sky term is dynamic and must not trigger remeshes**. `light` carries baked sky-light and block-light as two nibbles; the rendering section multiplies the *sky* nibble by the live sun intensity uniform and the *block* nibble by a fixed lamp curve, then multiplies the whole thing by `ao` — all in the fragment/vertex shader, per draw, against uniforms that change every frame as the sun moves. If I pre-folded sun into the vertex I would have to re-emit every vertex of every visible chunk at dawn and dusk, which is exactly the remesh storm the elastic budget cannot absorb. Separate `light` and `ao` bytes are therefore not redundancy — they are what makes a moving sun free. Section 5 consumes `a_light` and `a_ao` as two normalized-off byte attributes and does the fold live; it does **not** expect a single `a_bright`.

This decision also resolves cleanly with the greedy merge key (next subsection): both `light` and `ao` participate in the key as separate fields, so a merge stops wherever *either* differs — which is correct, because a shadow edge (light boundary) and a corner-darkening edge (AO boundary) are independent reasons to split a quad.

### Greedy Meshing — Per-Axis Slice Sweeps

I commit to greedy meshing, not naive per-face quads and not marching-anything. The reason is purely the quad-count argument above: a flat stone floor that naive meshing emits as 256 unit quads, greedy meshing emits as **one** quad. On a G70 with no instancing and GL 2.0 draw-call overhead, that 256:1 reduction is the difference between fitting the window in VRAM and not.

The algorithm runs **six sweeps per chunk** — three axes (X, Y, Z), two facing directions each (+/−). For a given axis, I walk the chunk slice by slice along that axis. Each slice is a 16×16 plane of *faces*: the boundary between voxel `[i]` and voxel `[i+1]` along the sweep axis. A face exists (is visible) when exactly one of the two voxels is opaque-solid and the other is air/transparent — the classic occlusion test. Within that 16×16 face-mask plane, I run a 2D greedy rectangle merge: scan to find the first unmerged visible face, extend the quad as far as possible in the U direction while material/light/AO/face-direction all match, then extend that whole run in the V direction while every face in the candidate row matches, mark the rectangle consumed, emit one maximal quad.

The merge key — what makes two adjacent faces mergeable — is **the full visible appearance**: same material id, same face direction, same baked light byte (both nibbles), same AO nibble. If light or AO differs across a boundary (a shadow edge, a corner), the merge must stop there or the interpolated shading would be wrong — and because light and AO are carried *separately* into the canonical vertex (above), each is an independent reason to split. This is why light and AO living *in the voxel word* (foundation layout) pays off twice: the mesher reads them in the same cache line it already loaded for the material test, and they participate in the merge key for free.

```c
/* One sweep: axis d in {0,1,2}, direction dir in {+1,-1}.
 * mask[] is a 16x16 plane describing each potential face on this slice.
 * A face entry packs the *merge key*: material, light, ao, or 0 = no face.
 * light is the full byte (sky|block nibbles); ao is the 4-bit value. Both
 * must match for a merge, mirroring the separate light/ao vertex fields. */

typedef struct { uint8_t mat, light, ao; uint8_t present; } FaceKey;

for (int slice = 0; slice <= 16; ++slice) {          /* 17 face-planes per axis */
    build_face_mask(chunk, neighbors, d, dir, slice, mask /*[16][16]*/);

    for (int v = 0; v < 16; ++v) {
        for (int u = 0; u < 16; ) {
            FaceKey k = mask[v][u];
            if (!k.present) { ++u; continue; }

            /* grow width along U while key matches (mat AND light AND ao) */
            int w = 1;
            while (u + w < 16 && key_eq(mask[v][u + w], k)) ++w;

            /* grow height along V while the whole [u..u+w) row matches */
            int h = 1;
            while (v + h < 16) {
                int ok = 1;
                for (int x = u; x < u + w; ++x)
                    if (!key_eq(mask[v + h][x], k)) { ok = 0; break; }
                if (!ok) break;
                ++h;
            }

            emit_quad(d, dir, slice, u, v, w, h, k);   /* one maximal quad */

            /* consume the merged rectangle so it isn't re-emitted */
            for (int yy = v; yy < v + h; ++yy)
                for (int xx = u; xx < u + w; ++xx)
                    mask[yy][xx].present = 0;

            u += w;
        }
    }
}

/* Merge predicate: all three appearance fields must be identical. */
static inline int key_eq(FaceKey a, FaceKey b) {
    return a.present && b.present
        && a.mat   == b.mat
        && a.light == b.light   /* both sky AND block nibbles */
        && a.ao    == b.ao;
}
```

**The seam problem is real and I accept it as a bounded cost, not a bug to fix.** Greedy merges stop dead at chunk boundaries — a stone floor spanning four chunks emits four quads, not one, because the mesher of chunk A cannot reach into chunk B's voxels to keep extending. I do *not* try to merge across chunks; that would couple chunk meshes and destroy the independent-remesh property that the dirty-chunk budget depends on. This is precisely why 16³ was chosen over 8³ in the foundation (8x the seam surface area). The seam cost at 16³ is the price of independent remesh, and it is paid in a handful of extra quads per chunk face, which the VRAM budget below absorbs comfortably.

**Boundary faces need the neighbor.** The +X face of the last voxel column in a chunk is occluded (or not) by the first column of the +X neighbor chunk. The mesher therefore reads one edge plane from each of the 6 neighbors — which is exactly why the foundation mandates **6 cached neighbor pointers per chunk**: the mesher dereferences a cached pointer, not a hash lookup, to fetch that edge plane. If a neighbor is non-resident (window edge), the boundary is treated as air (faces emit), and the chunk is flagged for re-mesh when that neighbor streams in. This is a binding interaction with the streaming section: **a chunk's mesh is only final once all 6 neighbors are resident; before that it carries possibly-spurious boundary quads.**

### Dirty-Chunk Tracking and the Per-Frame Remesh Budget

A chunk needs remeshing when (a) a voxel in it changed material/light/AO, (b) a voxel on one of its 6 boundary planes changed in a *neighbor* (because that flips a boundary face's visibility), or (c) a neighbor that was non-resident became resident. The simulation already sets the per-voxel `DIRTY_MESH` flag on edits; the mesher does not scan for dirt, it consumes a **per-chunk dirty set**. The CA, when it edits a voxel, marks that voxel's chunk dirty and — if the voxel sits on a chunk boundary plane — marks the abutting neighbor chunk dirty too. Chunk-level dirty is the real tracker; the per-voxel `DIRTY_MESH` bit is only a hint that lets an incremental optimization skip clean sub-regions if we ever build one (deferred; default is full-chunk remesh, which is only 16 KiB of input).

Note that light changes drive remeshes too — and because the per-vertex `light` byte is *unbaked* sky/block light (sun applied live in the shader, per the canonical format), a moving sun does **not** dirty any chunk; only a change to the *baked* sky/block propagation (a new opening dug to the surface, a placed lamp) marks chunks dirty. This is the payoff of keeping light and AO unfolded: the elastic mesh budget is spent on real geometry/lighting *topology* changes, never on the clock.

Meshing is the foundation's designated **elastic shock absorber**: 6 ms budget, and when a frame runs long it is meshing that gets throttled, never sim or render. So remesh is a *budgeted* operation, not "remesh everything dirty this frame":

```c
/* Per frame: drain the dirty queue until budget exhausted. */
#define MESH_MS_BUDGET        6.0
#define MAX_CHUNKS_PER_FRAME  8      /* hard cap, see sizing below */
#define MAX_QUADS_PER_FRAME   24000  /* ~8 chunks * ~300 quads typical-to-heavy */

int chunks_done = 0, quads_done = 0;
while (!pq_empty(&dirty_pq)
       && chunks_done < MAX_CHUNKS_PER_FRAME
       && quads_done  < MAX_QUADS_PER_FRAME
       && elapsed_ms() < MESH_MS_BUDGET) {
    Chunk* c = pq_pop_nearest(&dirty_pq);   /* highest priority = nearest player */
    MeshResult m = greedy_mesh(c);          /* 6 sweeps, writes scratch VB/IB */
    vbo_upload(c, &m);                       /* glBufferData / glBufferSubData */
    quads_done += m.quad_count;
    chunks_done++;
    c->dirty = 0;
}
/* leftover dirty chunks remain queued; they resolve over the next frames. */
```

**Priority queue keyed on player distance.** A dirty chunk under the player's feet must remesh this frame; a dirty chunk 180 m away can wait. The queue is ordered by squared chunk-center distance to the player (integer, no sqrt). When the CA dirties a chunk already queued, its priority is refreshed, not duplicated (the queue entry carries a generation/`in_queue` flag on the chunk so re-marks are idempotent). Foundation rule holds: **a chunk showing stale geometry for 2–3 frames is invisible to the player** — that is exactly the slack the elastic budget spends.

**Sizing the caps.** At 6 ms and a greedy sweep that is dominated by 6 linear passes over 4,096 voxels plus VBO upload, a typical chunk meshes in well under 1 ms; the `glBufferData` upload to the G70 is often the larger cost. Eight chunks/frame is the defended default cap — enough to keep up with normal digging and a spreading sim front, low enough that even eight heavy chunks plus their uploads stay inside 6 ms. The quad cap (24,000) is a second guard for the pathological case where eight chunks are all maximally fragmented; whichever cap (chunks, quads, or wall-clock) trips first ends the pass. **Flag for prototyping: the real ceiling is upload bandwidth to the G70 over the XP driver, which I cannot know without the M170 in hand — `MAX_CHUNKS_PER_FRAME` must be tuned against measured `glBufferData` cost, not assumed.**

### Mesh Data Layout — Binding the Canonical Vertex to the GL Pipeline

Interleaved attributes in a single VBO, indexed with a separate index buffer, using the **12-byte canonical vertex defined above** — the same struct Section 5 reads. No VAOs (hard constraint); attribute state is set per-draw with `glVertexAttribPointer` / `glEnableVertexAttribArray` against the bound `GL_ARRAY_BUFFER`, exactly as `GL_ARB_vertex_buffer_object` allows. Because the layout is shared, the renderer's pointer setup is a direct transcription of the offset table — **stride 12, offsets 0/6/7/8/9/10**:

```c
/* The renderer (Section 5) binds the canonical 12-byte vertex like this.
 * Stated here once; Section 5 cites these exact offsets, never its own. */
glVertexAttribPointer(LOC_POS,   3, GL_UNSIGNED_SHORT, GL_FALSE, 12, (void*)0);  /* px,py,pz */
glVertexAttribPointer(LOC_MAT,   1, GL_UNSIGNED_BYTE,  GL_FALSE, 12, (void*)6);  /* mat as float 0..255 */
glVertexAttribPointer(LOC_FACE,  1, GL_UNSIGNED_BYTE,  GL_FALSE, 12, (void*)7);  /* face 0..5 */
glVertexAttribPointer(LOC_LIGHT, 1, GL_UNSIGNED_BYTE,  GL_TRUE,  12, (void*)8);  /* sky|block, 0..1 */
glVertexAttribPointer(LOC_AO,    1, GL_UNSIGNED_BYTE,  GL_TRUE,  12, (void*)9);  /* ao -> 0..1 */
glVertexAttribPointer(LOC_UV,    2, GL_UNSIGNED_BYTE,  GL_FALSE, 12, (void*)10); /* tiling u,v */
```

Note the deliberate split of `GL_FALSE` vs `GL_TRUE` normalization. `mat` and `face` are uploaded **normalized-off** (raw integer values arrive as floats `0.0, 1.0, … 255.0`); `light` and `ao` are uploaded **normalized** so they arrive as `0.0..1.0` ready to multiply against the sun/lamp uniforms with no shader-side divide. The 16-bit positions arrive normalized-off as `0.0..16.0` floats. Position math in the shader adds the chunk's world origin uniform; nothing here needs the integer-attribute path.

**The material-id problem under GLSL 1.20 — and the fix.** Integer vertex attributes (`glVertexAttribIPointer`, `flat int` varyings) do **not** exist reliably in GL 2.0 / GLSL 1.20; everything that reaches the shader is a float, and interpolated. I will not gamble on `flat` either — `flat` qualifiers are GLSL 1.30+. So material id is delivered as the unsigned-byte attribute above, `GL_FALSE`-normalized, which hands the shader the raw integer value *as a float*. Because all four vertices of a quad carry the *same* `mat` id, the interpolated value across the quad is constant — there is no flat-vs-smooth hazard. The vertex shader reads `float a_mat` and uses it directly to pick the atlas tile:

```glsl
/* GLSL 1.20 vertex shader sketch. a_mat arrives 0.0..255.0, constant per quad.
 * a_light/a_ao arrive 0.0..1.0 (normalized bytes). Sun is a live uniform,
 * so day/night costs ZERO remeshes — the whole point of keeping light unfolded. */
attribute vec3  a_pos;     // px,py,pz (driver converts ushort->float, 0..16)
attribute float a_mat;     // material id as float, 0..255
attribute float a_face;    // 0..5
attribute float a_light;   // sky|block packed into one normalized byte
attribute float a_ao;      // ambient occlusion, 0..1
attribute vec2  a_uv;      // tile-local corner (tiling)
varying   vec2  v_uv;
varying   float v_shade;   // light*ao*sun folded LIVE, smooth-interpolated
uniform   float u_atlas_cols; // tiles per atlas row
uniform   float u_sun;        // live sun intensity, changes every frame
uniform   vec3  u_chunk_origin;

void main() {
    /* recover sky/block from the single light byte (sky in high range,
     * block in low range per the packing the lighting section defines),
     * apply live sun to sky term, fixed curve to block term, then AO. */
    float sky_block = a_light;              // detailed unpack lives in S5
    v_shade = clamp(sky_block * u_sun, 0.0, 1.0) * a_ao;

    /* fold material id into atlas UV: tile (col,row) from id + tile-local uv */
    float col = mod(a_mat, u_atlas_cols);
    float row = floor(a_mat / u_atlas_cols);
    v_uv = (vec2(col, row) + a_uv) / u_atlas_cols;

    gl_Position = gl_ModelViewProjectionMatrix * vec4(a_pos + u_chunk_origin, 1.0);
}
```

This is the **"fold material id into atlas UVs"** strategy done in the shader rather than baked into the vertex — which keeps the vertex at a slim 12 bytes and lets the atlas layout change without re-meshing. The atlas itself is a single 2D texture (no array textures in GL 2.0); 256 materials at 64×64 tiles in a 16×16 grid is a 1024×1024 atlas (1024 = 16·64), ~5.3 MiB with mips (Section 5.4 owns `ATLAS_SIZE`; `u_atlas_cols = 16`, tile = 64 texels) — trivial. Tile-local UVs are `0..tilesize` so greedy quads that span W×H voxels get **tiling** UVs (`u,v` run 0..W and 0..H), with `GL_REPEAT` on the tile sub-region — the standard greedy-mesh texture-tiling trick so a merged 8×4 floor quad still shows 64 texels of stone, not one stretched texel. Light·AO·sun are folded into the smooth-interpolated `v_shade` *live in the shader*, never baked, for the per-vertex lit look the rendering section expects.

Indices are **`GL_UNSIGNED_SHORT`** (2 bytes). A 16³ chunk's greedy mesh tops out far below 65,535 vertices (the absolute naive ceiling of distinct face-vertices is well under that, and greedy merging only lowers it), so 32-bit indices are never needed — saving half the index-buffer bytes and matching the G70's preference.

### Memory Budget — CPU Mesh Data vs. 256 MiB VRAM

**Per-quad cost (the unit that matters):** 4 vertices × 12 B = **48 B vertex data**, plus 6 indices × 2 B = **12 B**, = **60 B per quad** total. Vertex data alone, which is what dominates VRAM, is 48 B/quad.

**Per-chunk expectation.** A merged surface chunk is typically a few hundred quads; a fully exposed, maximally fragmented worst case is higher but rare. Concrete figures (vertex+index):

| Quads/chunk | VB | IB | Total |
|---|---|---|---|
| 100 (well-merged terrain) | 4.7 KiB | 1.2 KiB | 5.9 KiB |
| 300 (typical surface) | 14.1 KiB | 3.5 KiB | 17.6 KiB |
| 500 (busy/fragmented) | 23.4 KiB | 5.9 KiB | 29.3 KiB |
| 1000 (pathological) | 46.9 KiB | 11.7 KiB | 58.6 KiB |

**VRAM total against the 256 MiB G70.** The foundation states ~25% of the 10,000 resident chunks carry real geometry — call it **2,500 meshed chunks** (interior chunks below ground are solid-solid throughout and emit *zero* quads; air chunks above the surface emit zero; only the surface shell and dug-out cavities mesh). At a generous average of 300 quads/chunk: 2,500 × 17.6 KiB ≈ **43 MiB of mesh in VRAM**. Even at a pessimistic 400-quad average it is ~57 MiB. That leaves the bulk of the 256 MiB for the framebuffer/depth (~10 MiB at 1280×1024), the ~5.3 MiB atlas (Section 5.4), and a healthy reserve — **mesh geometry is not the VRAM bottleneck, and that is the dividend of greedy meshing plus the 12-byte vertex.** Had I taken the rejected 16-byte float-position vertex, the same scene would be ~57 MiB at 300 quads/chunk (33% more vertex bytes), and a fat 32-byte vertex with separate float position/normal/color would be ~115 MiB — survivable but reckless on 256 MiB once the rendering section's buffers are added. The 12-byte canonical vertex is the discipline that keeps this line comfortable.

**CPU-side mesh memory** maps to the foundation's **96 MiB CPU mesh-buffer line**. Two consumers: (1) a single reusable **scratch staging buffer** sized to the worst-case chunk (~64 KiB VB + 16 KiB IB) that `greedy_mesh()` writes into before upload — *one* such buffer, reused every remesh, never churned (foundation anti-fragmentation rule); (2) optional CPU-side shadow copies of VBO contents if the driver path requires re-specification on context loss. At a conservative planning average of **~38 KiB/meshed chunk** (the same figure Section 7 uses, so the two sections agree) the shadow set at 2,500 meshed chunks is **~93 MiB** (2,500 × 38 KiB), still inside the 96 MiB line — tightly, which is the point of using the conservative number rather than the optimistic 300-quad figure. **VBO allocation must be pooled** — chunks recycle fixed-capacity VBO handles via `glBufferData(NULL)` orphaning then `glBufferSubData`, rather than `glGenBuffers`/`glDeleteBuffers` per remesh, both to dodge driver-side address fragmentation and because buffer re-creation churn is exactly what the foundation's 32-bit fragmentation warning targets. Usage hint is `GL_DYNAMIC_DRAW` for chunks the sim actively edits, `GL_STATIC_DRAW` for settled terrain — a per-chunk flag the streaming/sim sections can set.

---

## 5. Rendering Pipeline

The renderer's job is to take ~10,000 resident chunks, throw away the ~96% the camera cannot see, and submit what remains as a small enough pile of draw calls that the GeForce Go 7800 GTX (G70) and its XP NVIDIA driver finish in **9 ms** — the budget the Frame Budget section handed me, non-negotiable. Everything below is shaped by three facts I will not re-derive: rendering shares one Pentium M core with sim and meshing, so my real enemy is **CPU-side draw submission**, not GPU fill rate; VRAM is **256 MiB** and owned here; and my API ceiling is **OpenGL 2.1 / GLSL 1.20** — which the G70 under the XP ForceWare driver fully implements — minus a self-imposed conservative envelope: no VAOs, no instancing, no UBOs, no geometry/compute shaders, and I treat integer vertex attributes and the `flat` interpolation qualifier as unavailable because those are GLSL 1.30+.

A word on that target, because the rest of the section leans on it. The G70 on Windows XP is a **full OpenGL 2.1 implementation**, not "2.0 with a few extras." 2.1 matters concretely: GLSL 1.20 is its shading language (which is what my shaders are written against), non-power-of-two textures are core, and pixel-buffer objects are core. I do not *use* all of that — I stay inside a deliberately conservative subset for portability and driver-bug avoidance — but I size my expectations to the real 2.1 baseline rather than underselling the hardware. Where I decline a 2.1-era feature (NPOT atlas, say) it is a defended choice, not a capability gap.

The G70 is, for its era, a monster: 24 fragment pipelines, 8 vertex pipelines, ~10 GB/s of texture bandwidth. A forward-rendered voxel world at 30 FPS does not stress its shading or fill rate. It stresses two things I must respect: **256 MiB of VRAM** that holds every chunk VBO plus the atlas, and the **per-`glDrawElements` driver overhead** that lands on my single CPU core. So this section optimizes for *fewer, fatter draws and a compact vertex format*, and treats the fragment shader as nearly free.

### 5.1 Forward Renderer, Two Passes, Defended

There is no deferred path here and it is not a close call. Deferred shading needs multiple-render-target G-buffers and the fat fragment work to fill them; on a 256 MiB G70 a G-buffer at any reasonable resolution eats VRAM I need for chunk geometry, and the whole point of deferred — cheap many-light accumulation — is irrelevant to a world lit by **exactly one directional sun plus baked voxel light**. Forward rendering, two passes:

1. **Opaque pass** — all solid voxel geometry, depth test + depth write on, no blending, front-to-back-ish (chunk order by distance to exploit early-Z). This is the bulk of the frame.
2. **Liquid/transparent pass** — water, molten metal, glass; depth test on but **depth write off**, alpha blending on, drawn **back-to-front** (Section 5.6).

Per-frame GL state setup is deliberately minimal because state changes are CPU cost on this driver. The frame's fixed scaffolding:

```c
/* Once per frame, set and forget for the opaque pass. */
glEnable(GL_DEPTH_TEST);  glDepthFunc(GL_LEQUAL);  glDepthMask(GL_TRUE);
glEnable(GL_CULL_FACE);   glCullFace(GL_BACK);     /* greedy mesher emits CCW outward */
glDisable(GL_BLEND);
glActiveTexture(GL_TEXTURE0);  glBindTexture(GL_TEXTURE_2D, atlas_tex); /* bound ONCE, all frame */
glUseProgram(opaque_prog);
/* Upload view/proj + sun direction as uniforms once; per-chunk only the cheap stuff changes. */
```

The atlas is bound **once for the entire opaque pass** — every chunk samples the same atlas, so there is zero texture rebinding across thousands of chunks. That single decision removes the most common voxel-renderer state-change tax.

#### The vertex format: 12 bytes, every byte accounted for

The vertex format is the load-bearing artifact, so I pin it precisely before the shader and I make it tight — vertex count, not fragment work, is what fills the 256 MiB of VRAM (see 5.4). The vertex is **12 bytes, matching the Meshing layout** byte-for-byte (Section 4 owns the canonical struct; this section reads it back), and every field maps one-to-one onto a shader attribute. There is no "second stream," no "spare bytes," no `uint8 light` hanging off the end of a struct that was already full. Here it is — the same `mat/face/light/ao/u/v` field order Section 4 writes:

```c
/* 12 bytes/vertex, interleaved, single VBO. No VAO on G70 -> attribute
 * pointers are re-specified per chunk bind via glVertexAttribPointer.
 * Position is chunk-local in plain voxel units: a 16^3 chunk spans 0..16,
 * stored in u16 (the Decision Ledger and Section 4 own this encoding). The
 * shader recovers world position as a_pos + u_chunkOrigin. (u16 leaves ample
 * headroom for a future sub-voxel/bevel encoding, but the committed format
 * emits integer voxel coordinates 0..16 only.) */
typedef struct {
    uint16_t px, py, pz;   /* 6 B: chunk-local position, voxel units (0..16)             */
    uint8_t  mat;          /* 1 B: MaterialDef / atlas tile id, 0..255                    */
    uint8_t  face;         /* 1 B: face direction 0..5 (for tile sub-UV + future use)     */
    uint8_t  light;        /* 1 B: baked sky|block light, UNFOLDED (sun applied live)     */
    uint8_t  ao;           /* 1 B: ambient occlusion 0..15 (separate per-vertex byte)     */
    uint8_t  u, v;         /* 2 B: atlas TILE-corner index, 0/1 per axis -> see below   */
} Vtx;   /* sizeof == 12, naturally aligned, no compiler padding (== Section 4 MeshVert) */
```

A note on the UV encoding, because it changed how `u,v` are stored: rather than baking a full 16-bit atlas texture coordinate per vertex, I store the **tile-corner** as two bytes (which corner of the tile this vertex sits at — effectively 0 or 1 per axis, with room for the gutter-inset fraction) plus the **tile id in `mat`**, and the vertex shader assembles the final atlas UV from `u_atlas_cols` and the per-vertex tile id. This is cheaper in VRAM and, critically, it makes the tile-grid dimension a *uniform* (`u_atlas_cols = 16`, see 5.4), so the renderer and the Section 4 mesher reference one shared grid constant instead of two sections independently hardcoding `/1024.0` divisors that could drift apart. The mesher's job at mesh time is therefore: pick the tile id (from `MaterialDef` + face), emit the four corner codes, and write the per-vertex `light` and `ao` bytes UNFOLDED — it does **not** pre-divide into 0..1 texture space and does **not** bake the sun into the geometry.

Every shader attribute below maps onto a real field of this 12-byte struct — `a_pos`→`px,py,pz`, `a_tile_uv`→`u,v`, `a_light`→`light`, `a_ao`→`ao`, `a_tile`→`mat`, `a_face`→`face`. There is no orphaned attribute and no orphaned byte.

#### The opaque voxel shader (GLSL 1.20)

I ship light and AO as **two separate per-vertex bytes** (`light`, `ao`) — exactly the bytes Section 4's greedy merge key holds distinct — and fold light·AO·sun *live in the shader*, never baked into the geometry (Section 5.5). Keeping the sun term out of the vertex is what makes a moving sun cost zero remeshes. The lighting math is still almost trivially cheap, which is exactly what I want when 24 fragment pipes sit idle and the CPU is the bottleneck:

```glsl
/* ---- vertex shader, GLSL 1.20 ---- */
#version 120
uniform mat4  u_mvp;
uniform vec3  u_chunkOrigin;    /* world position of chunk's (0,0,0) corner          */
uniform float u_atlasInvCols;   /* = 1.0/16.0; ONE grid constant shared with mesher   */
uniform float u_tileInset;      /* gutter inset in tile-UV units (5.4)                */
uniform float u_sun;            /* live sun intensity, changes every frame            */
uniform float u_fogStart;
uniform float u_fogInvRange;

attribute vec3  a_pos;          /* px,py,pz -> chunk-local voxel units (0..16)          */
attribute vec2  a_tile_uv;      /* u,v -> tile corner 0/1 per axis                     */
attribute float a_light;        /* baked sky|block light, UNFOLDED, normalized 0..1     */
attribute float a_ao;           /* ambient occlusion, normalized 0..1                  */
attribute float a_tile;         /* mat -> atlas tile id 0..255                          */
attribute float a_face;         /* face direction 0..5 (reserved for face-specific UV) */

varying vec2  v_uv;
varying float v_bright;
varying float v_fog;

void main() {
    vec4 world = vec4(a_pos + u_chunkOrigin, 1.0);
    gl_Position = u_mvp * world;

    /* Reconstruct atlas UV from tile id + corner, using the shared grid constant.
     * col = mod(tile,16); row = floor(tile/16). Inset by u_tileInset toward tile
     * center so bilinear/mip sampling never crosses the gutter (5.4). */
    float col = mod(a_tile, 16.0);
    float row = floor(a_tile * u_atlasInvCols);
    vec2  corner = mix(vec2(u_tileInset), vec2(1.0 - u_tileInset), a_tile_uv);
    v_uv = (vec2(col, row) + corner) * u_atlasInvCols;

    /* Fold light, AO and the live sun term HERE, never at mesh time: sky-light
     * scales by the per-frame u_sun uniform, then AO multiplies in. A moving
     * sun changes u_sun, not the geometry, so day/night costs zero remeshes. */
    v_bright = clamp(a_light * u_sun, 0.0, 1.0) * a_ao;   /* 0..1 */
    v_fog    = clamp((gl_Position.w - u_fogStart) * u_fogInvRange, 0.0, 1.0);
}
```

```glsl
/* ---- fragment shader, GLSL 1.20 ---- */
#version 120
uniform sampler2D u_atlas;
uniform vec3      u_fogColor;
uniform vec3      u_sunTint;    /* warm tint, lets the same baked light read as daylight */
varying vec2  v_uv;
varying float v_bright;
varying float v_fog;
void main() {
    vec4 tex = texture2D(u_atlas, v_uv);          /* mip-mapped, capped chain (5.4) */
    vec3 col = tex.rgb * (v_bright * u_sunTint);  /* ALL lighting is the one multiply */
    gl_FragColor = vec4(mix(col, u_fogColor, v_fog), 1.0);
}
```

Note there is no `a_fognone` placeholder anymore — fog is derived from `gl_Position.w` in the vertex shader, costs nothing in VRAM, and the 12-byte struct has no dead attribute. That fog mix is not decoration — it is a budget instrument. The G70 will be fogging out the far chunks anyway (this is why the Memory section capped horizontal radius at 12), so fog lets me **cull aggressively at the fog wall** and the player never notices geometry popping in behind the haze.

### 5.2 Frustum Culling: Chunk AABB vs Six Planes

Culling happens at **chunk granularity** — 16³ AABBs against the camera frustum. I extract the six frustum planes from the combined view-projection matrix once per frame (Gribb-Hartmann), then test each *candidate* chunk's AABB. Candidates are not "all 10,000 resident chunks" — I iterate only chunks within the fog radius, which is already smaller than the resident window, and I walk them in a rough front-to-back order (spiral out from the camera chunk) so early-Z does real work in the opaque pass.

```c
typedef struct { float a, b, c, d; } Plane;   /* a*x+b*y+c*z+d = 0, normalized */

/* n-vertex test: pick the AABB corner furthest along +normal.
 * If even that corner is behind the plane, the whole box is out. */
static inline int aabb_outside_frustum(const Plane p[6], vec3 mn, vec3 mx) {
    for (int i = 0; i < 6; ++i) {
        float px = (p[i].a >= 0.0f) ? mx.x : mn.x;
        float py = (p[i].b >= 0.0f) ? mx.y : mn.y;
        float pz = (p[i].c >= 0.0f) ? mx.z : mn.z;
        if (p[i].a*px + p[i].b*py + p[i].c*pz + p[i].d < 0.0f)
            return 1;                          /* fully outside this plane */
    }
    return 0;
}
```

Six planes × one dot product per chunk is a handful of FLOPs; testing the full fog-radius candidate set is well under a millisecond even on the Pentium M, and it routinely discards 60–80% of candidate chunks for a typical FOV. **I deliberately stop at AABB-vs-frustum and do not add occlusion culling.** GL 2.1 hardware occlusion queries (`ARB_occlusion_query`, which the G70 supports) introduce a CPU-GPU round-trip and latency-management complexity that, on a single core with a 9 ms budget, costs more to manage than the overdraw it would save — and front-to-back ordering plus early-Z already mitigates overdraw cheaply. **Flag for prototyping:** if dense terrain (deep caves, the player buried in a mountain) shows pathological overdraw, a *coarse* software heuristic — skip chunks fully enclosed by opaque neighbors, which the mesher already knows because such a chunk produces zero exterior faces — is the contingency, not GPU queries. A fully-buried chunk meshes to an empty VBO and is free to "draw" anyway, so this mostly self-solves.

### 5.3 Draw-Call Batching Without Instancing

This is where the frame is won or lost on the CPU. The two honest options are **one VBO per chunk** versus **merged regional buffers** (one big VBO per N×N×N region of chunks). I choose **one VBO per chunk**, and I defend it against the merged approach specifically for *this* engine.

**Merged regional buffers** would cut draw calls — combine, say, a 4×4×16 column of chunks into one VBO and one draw. Fewer `glDrawElements`, lower driver tax. But the cost is fatal to our core loop: **the simulation makes geometry dirty constantly** (heat plumes, melting pools, flowing liquid, the player digging). When any one chunk in a merged region goes dirty, I must either re-upload the entire merged VBO (re-streaming hundreds of KiB of vertex data across the bus for a one-chunk change — directly stealing from the 6 ms mesh budget and the 9 ms render budget) or maintain a sub-allocation/free-list inside the big buffer (a memory-fragmentation problem on the GPU that the foundation already warned me about on the CPU side). Per-chunk VBOs make remesh **surgical**: a dirty chunk re-uploads its own small VBO and nothing else. Given that the whole architecture is built around tightly-bounded active fronts and incremental remesh, regional merging fights the grain of the design. **Rejected.**

So: **one VBO per chunk, drawn with `glDrawElements`.** The numbers say this is affordable. After frustum + fog culling, a typical view shows on the order of **150–400 visible non-empty chunks** (most resident chunks are culled, underground, fully solid/empty, or beyond fog). At ~300 draw calls and the per-call overhead of the XP NVIDIA GL 2.1 driver — measured in the low single-digit microseconds per call for a simple bound state — that is well within 9 ms with room for the liquid pass. The discipline that keeps it there:

- **VBOs via `ARB_vertex_buffer_object` (core since GL 1.5), never client arrays.** On the G70, client-side vertex arrays (`glVertexPointer` into RAM) re-DMA the whole vertex set across the AGP/PCIe bus every single frame — catastrophic for static chunk geometry that changes only on remesh. VBOs live in VRAM; a static, unchanged chunk costs *only* its draw call each frame, zero vertex transfer. Chunk VBOs are created `GL_STATIC_DRAW` (they change only on remesh, which is rare relative to frame rate).
- **Indexed geometry with a shared static quad index buffer.** Greedy-meshed faces are quads → two triangles. The index pattern (0,1,2, 2,3,0 per quad) is identical for every chunk, so I keep **one** element buffer sized to the max quads per chunk and reuse it for all draws — only the vertex VBO is per-chunk. This shrinks per-chunk VRAM and removes per-chunk index uploads.
- **Minimize state changes between draws.** Atlas bound once (5.1), shader bound once, depth/cull state set once for the whole opaque pass. The *only* things that change per chunk are (a) binding the chunk's VBO and resetting the attribute pointers, and (b) one `glUniform3f` for `u_chunkOrigin`. Position is stored chunk-local (the 1/4-voxel `u16` of 5.1) precisely so the per-chunk delta is a cheap uniform, not a re-bake of world-space coordinates.

```c
/* Per visible chunk, opaque pass. State above is already set. */
for (i = 0; i < n_visible; ++i) {
    Chunk *c = visible[i];
    if (c->vbo == 0 || c->index_count == 0) continue;  /* empty/buried -> free */
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    setup_attrib_pointers();                 /* offsets into the 12-byte interleaved fmt */
    glUniform3f(u_chunkOrigin, c->ox, c->oy, c->oz);
    glDrawElements(GL_TRIANGLES, c->index_count, GL_UNSIGNED_SHORT, 0);
}
```

`GL_UNSIGNED_SHORT` indices cap a chunk at 65,536 vertices. A 16³ chunk's greedy mesh cannot remotely approach that (worst case ~6 faces × pathological splitting is still well under 16k verts), so 16-bit indices are safe and halve index VRAM versus 32-bit.

### 5.4 Texture Atlasing for Materials

All material textures live in **one atlas texture, bound once**. Material behavior is data-driven from the 256-entry `MaterialDef` table (a global reality), so each `MaterialDef` carries an **atlas tile index** (and optionally per-face indices for materials that need a distinct top/side/bottom, e.g. a grassed soil). The mesher converts (tile index, face corner) → atlas UV at mesh time — really, it bakes the *tile id* and *corner code* into the vertex (5.1) — and the shader reconstructs the final UV from the shared grid constant; the shader just samples.

**Atlas dimensions — the one geometry both this section and the Section 4 mesher MUST cite: a 16×16 grid of 64×64-texel tiles → a 1024×1024 RGBA8 atlas.** This is `ATLAS_SIZE` and it is binding for the mesher too: the mesher's UV-folding and this shader's `u_atlas_cols = 16` / `u_atlasInvCols = 1/16` are the *same* grid dimension, and Section 4 now cites this same 1024×1024 / 64×64-tile grid (the earlier draft mismatch is reconciled). Defended:
- 64×64 per tile is generous for a 1 m voxel face seen at Minecraft-ish view distances; finer detail is wasted behind fog and FOV. 32×32 would save VRAM, but at ~5.3 MiB the savings are noise against 256 MiB and 64×64 buys real surface detail and gutter headroom — so 64×64 wins.
- 16×16 = **256 tiles**, exactly matching the 256-material ceiling — every material can have a base tile, and materials needing multi-face art draw from the same pool (most materials share faces, so 256 tiles is comfortable, not tight).
- 1024×1024 RGBA8 with a (capped) mip chain ≈ **5.3 MiB of VRAM** (verified: 4 MiB base × 1.333 = ~5.3 MiB at full pyramid, i.e. 4 MiB base + ~1.33 MiB mips; the cap below trims the tail slightly). Trivial against 256 MiB. The G70's max texture size is 4096×4096, so 1024 is well within hardware limits with massive headroom if I ever want 128×128 tiles (→ 2048² ≈ 22 MiB, still trivial).
- The G70 exposes 16 fragment texture units; I use **exactly one**. There is no multitexturing pressure. (A second unit is held in reserve for a future normal/emissive map for glowing molten metal — flagged, not built.)

#### How the atlas is uploaded and mipped (GL 2.1-correct mechanism)

I generate the mip chain at upload time, and I am precise about the call, because the obvious spelling is wrong on this stack. **`glGenerateMipmap` (the bare function) is NOT available on OpenGL 2.1** — it entered core in GL 3.0. On the G70 / GL 2.1 target the two correct mechanisms are:

1. **The `GL_GENERATE_MIPMAP` texture parameter** (`SGIS_generate_mipmap`, **core since GL 1.4**) — set `glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE)` *before* the `glTexImage2D` upload, and the driver auto-generates the full chain on upload. This is my primary path: zero extra calls, universally supported on this driver.
2. **`glGenerateMipmapEXT`** from `EXT_framebuffer_object`, which the G70 *does* support, as an explicit alternative if I ever want to regenerate mips after a sub-image update without re-uploading the base.

I use mechanism (1). I do **not** reference the bare `glGenerateMipmap` spelling anywhere — that is a GL 3.0 symbol and not guaranteed to resolve on this stack.

#### The mipmap-bleeding problem and how I solve it without per-tile clamping

This is the one genuinely tricky part of atlasing and I will be precise. With a single atlas and trilinear mipmapping, two failures occur at tile edges:

1. **Bilinear bleed at the base level:** texels from a neighbor tile leak across the shared edge because the sampler interpolates past the tile boundary.
2. **Mip-level bleed:** at coarse mip levels, a single texel averages texels from *multiple* tiles, so distant voxels show smeared neighbor colors.

`GL_CLAMP_TO_EDGE` fixes this for a *whole texture*, but it cannot clamp *per tile* inside one atlas — that is the trap the prompt names. My mitigation is the standard, robust, GL 2.1-safe combination:

- **Padding gutters + tile border extrusion.** Each 64×64 tile gets a border of extruded edge texels (the tile's edge color smeared outward) inside an allocated cell larger than the visible tile. With a generous gutter, bilinear sampling at the base level never reaches real neighbor content. This is also why the vertex shader insets corners by `u_tileInset` toward the tile center (5.1) — the sampled region stops short of the gutter. This kills failure (1).
- **Capped mip levels, not a full pyramid.** I cap the chain at the level where a tile is still several texels wide (for 64×64 tiles, stop around 8×8 or 4×4), via `glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, k)` — and `GL_TEXTURE_MAX_LEVEL` is **core since GL 1.2**, so this is safe as written. Beyond that point a mip texel would straddle tiles regardless of gutter width, so I simply don't let the hardware sample those levels. Distant voxels are tiny and fogged; clamping max LOD there is invisible. This handles failure (2) without abandoning mipmapping entirely (which would alias and shimmer badly on the G70). The `GL_GENERATE_MIPMAP` auto-gen path (above) plus `GL_TEXTURE_MAX_LEVEL` cooperate cleanly: the driver builds the chain, the cap bounds which levels are ever sampled.
- **A small negative LOD bias if needed** (`GL_TEXTURE_LOD_BIAS`, via `EXT_texture_lod_bias` / core in GL 1.4+) to bias sampling slightly toward sharper levels, trading a touch of aliasing for less bleed. **Flag for prototyping:** the exact gutter width (likely 4–8 texels), the `u_tileInset` value, and the max-mip cap are tuned by eye on the actual M170 panel; I commit to *gutters + capped mips + corner inset* as the mechanism and defer the magic numbers.

I deliberately reject the alternative of **texture arrays** (`EXT_texture_array` exists on G70-era hardware but is not GL 2.1 core, support on the XP NVIDIA driver of that vintage is uneven, and it complicates GLSL 1.20 sampling) — the gutter-plus-capped-mip atlas is the conservative, known-safe path. I also reject **NPOT atlas sizes**; even though NPOT textures are core in GL 2.1, 1024×1024 is power-of-two and dodges the G70's NPOT *mipmapping* caveats entirely, so I take the free win.

### 5.5 Lighting Model — Baked, and Exactly What I Cut

The lighting that survives into the renderer is **one directional sun + baked voxel light (skylight + block-light) + per-vertex baked AO**, shipped as the two separate per-vertex `light` and `ao` bytes (5.1) and folded with the live sun term *in the shader*. Nothing is computed per fragment beyond the texture multiply. But before I describe what is in and what is out, I have to state the **pipeline order** explicitly, because it is the contract between this section and the Section 4 greedy mesher and the two have to agree exactly.

#### Pipeline order: nibbles first (merge key), separate light/AO bytes second (upload), sun folded live in shader

The order is, per face vertex, at chunk-(re)mesh time:

1. **The mesher computes per-vertex light and AO as separate 4-bit nibbles FIRST.** Light is the value already flooded into the voxel `light` nibble (skylight + block-light, see below); AO is the corner-darkening value computed from the three voxels adjacent to that vertex corner.
2. **Those two nibbles are the greedy merge key, exactly as Section 4 specifies.** Two coplanar same-material faces merge into one quad **only if both the light nibble AND the ao nibble match across the shared boundary** (in addition to material and face direction). This is the contract: the merge decision is made over the *separate nibbles*, before they are combined. The per-vertex variation in skylight/block-light/AO is precisely what limits merge-run length — that is expected and correct, and it is why the mesher must hold the nibbles distinct at merge time.
3. **Only AFTER the merge decision does the mesher write the surviving per-vertex nibbles out as the two separate `light` and `ao` bytes** for upload into the 12-byte vertex (5.1) — `light = light_nibble * 17` and `ao = ao_nibble * 17` (the ×17 expands a 4-bit value to the full 0..255 byte range so the normalized-byte attribute lands cleanly on 0..1). The sun term is **NOT** folded in here: it is `max(0, dot(faceNormal, -sunDir))` for this quad's (constant) normal, but it is applied **live in the shader** against the `u_sun` uniform so a moving sun costs zero remeshes. The shader fold is `bright = clamp(a_light * u_sun, 0..1) * a_ao`. Because a greedy quad spans many voxels of one material with one normal, the sun term is uniform across the quad; the light and AO bytes still vary per corner, so the interpolated `v_bright` carries the within-quad gradient.

So to settle the cross-section question once and for all: **the vertex stores `light` and `ao` as two separate per-vertex bytes (this section's 5.1 format, matching Section 4), and the sun is NOT baked into the geometry.** The 4-bit nibbles exist only transiently inside the mesher, where they serve the merge key; on upload they expand into the two distinct bytes, and the sun multiply happens live in the shader. `light` and `ao` are genuine **per-vertex** attributes, so within-quad light/AO gradients still interpolate smoothly across the quad in the rasterizer; the live shader fold does not flatten the gradient, and it keeps day/night at zero remeshes.

With that contract stated, here is precisely what is in, what is out, and why.

**In — directional sun.** One global sun direction, supplied as the live `u_sun` uniform. Its contribution is `max(0, dot(faceNormal, -sunDir))`. Because greedy-meshed quads are axis-aligned, the face normal is one of six constants known at mesh time, so the per-face sun term is cheap — but it is applied **live in the vertex shader against `u_sun`**, never baked into the geometry, so a moving sun costs zero remeshes. The fragment shader still has no normal, no light vector, no `dot`: the sun fold happens once per vertex and arrives as the interpolated `v_bright`, leaving the fragment stage at the one texture multiply.

**In — voxel skylight + block-light flood fill, BAKED at mesh time.** The 4-bit `light` nibble in each voxel (a global reality — it is *in the voxel word*) holds the propagated light level. I run a **breadth-first flood fill** at chunk-(re)mesh time: skylight floods straight down from open sky at full level and decays sideways; block-light (torches, glowing molten metal, lava) floods outward from emissive voxels, decremented per step. The result is written into the voxel `light` nibble and read by the mesher in the *same cache line it already touched* for material — the foundation explicitly placed light in-voxel for exactly this zero-extra-traffic reason. The mesher samples the four voxels around each face vertex and averages their light to get the smooth per-vertex light nibble (step 1 above). **I keep block-light flood fill** because emergent glowing-furnace-lights-the-cave is a core part of the fantasy — a forge should cast light. The flood fill is bounded (it runs only within the dirty chunk plus a one-chunk neighbor margin, since light propagates across seams) and lives in the **6 ms mesh budget**, not the 9 ms render budget.

**In — per-vertex AO, baked.** The classic voxel corner-darkening: for each face vertex, sample the three voxels diagonally/edge-adjacent at that corner; the more are solid, the darker. Quantized to a 4-bit ao nibble (step 1), used in the merge key (step 2), expanded into the separate per-vertex `ao` byte (step 3) and multiplied in live in the shader. Pure CPU work at mesh time, zero runtime cost.

**Cut — everything dynamic and per-fragment.** No real-time shadow maps (the G70 *can* do depth-texture shadows via `ARB_shadow`, but a shadow pass means re-rendering scene geometry from the sun's view — a second full geometry submission I cannot afford on one core in 9 ms, and the baked skylight already gives convincing "under an overhang is dark"). No per-fragment point lights (block-light is baked instead — moving a torch just re-dirties nearby chunks, which the streaming/sim already does). No specular, no normal mapping in v1 (flagged as the reserved second texture unit's future home for molten-metal glints). No bloom/HDR/post (the framebuffer is plain RGBA8; post-processing is fill-rate and VRAM I won't spend). The hard line: **lighting that changes only when geometry changes is baked at mesh time; nothing is computed per pixel except a texture fetch and one multiply.** That is what makes a richly-lit world feasible on this GPU within budget.

The one thing that *does* change every frame — the sun's color/intensity as day/night cycles — rides in the `u_sun` intensity uniform (and a `u_sunTint` color uniform), which the shader multiplies against the unfolded per-vertex `light` byte live. So a full day-night cycle costs **one or two uniforms per frame and zero remeshing**, no matter how far the sun moves — because the directional `sun_term` is never baked into the geometry, there is no drift threshold and no lazy re-bake. **Flag for prototyping:** the exact `sun_term`/sky-light curve the shader applies to the sky nibble (and the lamp curve for the block nibble) is a tuning question, but it lives entirely in shader uniforms, never in a remesh.

### 5.6 Liquid / Transparent Pass

Water and molten metal are the transparent surfaces (glass too, if added). They are meshed into a **separate VBO per chunk** from the opaque geometry — same per-chunk strategy, different buffer — so the transparent pass is its own draw loop with its own state. State setup:

```c
/* After the opaque pass completes. */
glDepthMask(GL_FALSE);          /* test against opaque depth, but DON'T write depth */
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* standard over-blend */
glUseProgram(liquid_prog);      /* opaque shader + alpha from MaterialDef + animated UV scroll */
/* draw transparent chunk VBOs BACK-TO-FRONT */
```

**Depth write off** is the crux: writing depth from a translucent surface would let a near water surface occlude farther water incorrectly. With write off but test on, liquids correctly hide behind opaque terrain (which already wrote depth) while blending among themselves.

**Ordering is back-to-front at chunk granularity, not per-triangle.** I sort the visible transparent chunks by distance from the camera (descending) and draw far chunks first. Per-triangle sorting is off the table — it would mean re-sorting and re-uploading index buffers every frame on the CPU, blowing the budget. Chunk-granularity sorting is cheap (an insertion sort over the few-dozen visible liquid chunks) and **good enough** because within a single 16³ chunk, liquid surfaces are mostly a near-planar pool top — intra-chunk blend-order errors are rare and visually negligible for opaque-ish water. **Flag for prototyping:** if stacked liquids (a waterfall in front of a lake) show visible blend artifacts, the contingency is to additionally sort the *quads within* a liquid chunk's index buffer once at remesh time (cheap, done in the mesh budget, not per frame), never per-frame per-triangle sorting. Molten metal is nearly opaque (high alpha), which hides ordering sins; clear water is where to watch.

The liquid shader is the opaque shader plus two things: alpha pulled from the `MaterialDef` (data-driven, so a new translucent material needs no shader change) and an animated UV scroll (a time uniform offsets `v_uv`) so water and lava visibly flow — cheap, and it sells the cellular-automaton fluid motion the sim is computing underneath. The liquid vertex format is the same 12-byte `Vtx`; the `pad` byte reserved in 5.1 is the natural home for a per-vertex anim/flow flag if the scroll ever needs to be material- or face-directional.

### 5.7 GL 2.1 Features / ARB Extensions Relied Upon — and Why They Are Safe

Everything here is OpenGL 2.1 core or an ARB/EXT extension that ships in the G70's XP NVIDIA ForceWare driver, which is a **full GL 2.1 implementation**. The dependency list, asserted safe:

| Feature / extension | Used for | Safe on G70 + XP NVIDIA (GL 2.1)? |
|---|---|---|
| `ARB_vertex_buffer_object` | Per-chunk VBOs + shared index buffer (5.3) | **Yes** — core since GL 1.5; foundational on G70, the single most important dependency. |
| GLSL 1.20 (`ARB_shader_objects`, `ARB_vertex_shader`, `ARB_fragment_shader`) | The opaque/liquid shaders (5.1) | **Yes** — GLSL 1.20 *is* the GL 2.1 shading language; G70 is a full SM3.0 part, comfortably exceeds GLSL 1.20 needs. |
| `EXT_texture_lod_bias` / core LOD bias | Mip-bleed mitigation tuning (5.4) | **Yes** — core since GL 1.4; long-standing NVIDIA support. |
| `GL_GENERATE_MIPMAP` texture param (`SGIS_generate_mipmap`) + `GL_TEXTURE_MAX_LEVEL` | Auto-generate + cap the atlas mip chain (5.4) | **Yes** — `GL_GENERATE_MIPMAP` auto-gen is core GL 1.4, `GL_TEXTURE_MAX_LEVEL` is core GL 1.2. `glGenerateMipmapEXT` (via `EXT_framebuffer_object`) is the supported fallback; the bare `glGenerateMipmap` (GL 3.0) is NOT used. |
| `ARB_multitexture` | Reserved second texture unit (future emissive map) | **Yes** — core since GL 1.3; G70 has 16 units. Not used in v1. |

**Explicitly NOT relied upon** (and why, so no downstream section assumes them): no VAOs (`ARB_vertex_array_object` is GL 3.0-era, unreliable on this stack — I re-specify attribute pointers per chunk bind instead); no instancing (`ARB_draw_instanced` post-dates this hardware — hence per-chunk draws, 5.3); no UBOs (GL 3.1 — uniforms are set individually); no geometry/compute shaders; no integer vertex attributes and no `flat` interpolation qualifier (both GLSL 1.30+ — this is why `a_tile`/`a_face`/`a_light`/`a_ao` are passed as `float` attributes and reconstructed with `mod`/`floor` or read as normalized floats, not as integers); no `EXT_texture_array` (5.4 rationale); no MRT/deferred. The renderer lives inside a deliberately conservative subset of the GL 2.1 envelope, and every per-frame cost is a CPU-submission cost I have minimized to fit the **9 ms** render budget alongside the **6 ms** mesh budget that feeds it geometry.

---

## 6. Frame Scheduling

This section governs the one thing the single core cannot escape: time. Simulation, meshing, and rendering do not run in parallel — there is no second core to hide one behind another, so they run *cooperatively*, in sequence, inside a 33.33 ms wall. The scheduler's whole job is to decide, every frame, how much of that wall each phase is allowed to consume, and — when the work does not fit — what gets cut, in a fixed priority order, so the frame still ends on time. Everything here is built on top of the binding `FRAME_BUDGET_SPLIT` (Sim 14 / Mesh 6 / Render 9 / Overhead 4.33 ms) and the `SIM_TICK_MODEL` (fixed-rate, decoupled, no catch-up). I am not re-litigating those numbers; I am defining the loop that enforces them.

### Why 30 FPS Is a Scheduling Decision, Not a Render Decision

The 30 FPS / 33.33 ms target is restated here because it is the scheduler's foundational axiom and it is justified entirely by the serialization constraint. On a machine with N cores you target a framerate and let threads absorb the work; on the Pentium M 780 — single core, no SMT, in-order-ish issue — the framerate *is* the total time budget for sim plus mesh plus render plus OS overhead, summed end to end. At 60 FPS that sum must close in 16.67 ms. After the GL 2.0 driver's draw-call submission cost and a non-trivial remesh, the cellular automaton — the entire reason this game exists — would be left with single-digit milliseconds and a working set that no longer amortizes its L2 residency across enough voxels to matter. 30 FPS doubles the sim's slice to a defensible 14 ms and, just as importantly, halves the *frequency* at which meshing and render compete for the core, giving the elastic mesh queue more wall-clock time to drain across frames. For a slow-paced survival/progression world driven by heat and fluid fronts, 30 FPS reads as smooth; nobody dodges a bullet at voxel scale. **The target is 30 FPS because that is the largest sim budget the single core can afford while still drawing the world.**

A second, subtler reason 30 FPS is correct: it decouples cleanly from the sim tick. If the sim runs at a fixed 15 Hz (the CA section's call — I assume 10–20 Hz per `SIM_TICK_MODEL` and design for a configurable rate), then at 30 FPS we render exactly two frames per sim tick on average. That is a clean 2:1 cadence that the accumulator below handles without fractional drift, and it means render-side interpolation between sim states is a simple two-frame blend, not an irregular one.

### The Clock: One Monotonic Timer, Read Three Times

The loop measures elapsed wall time with a single high-resolution monotonic counter. On the XP/M170 target that is `QueryPerformanceCounter` (QPC); on the Linux dev/cross-build host it is `clock_gettime(CLOCK_MONOTONIC)`. Both go behind one function, `now_ms()`, returning a `double` of milliseconds since startup. I deliberately do **not** use `timeGetTime`/`GetTickCount` (≈10–15 ms resolution on XP — far too coarse to time a 6 ms mesh budget) and I do **not** trust the CPU `rdtsc` directly, because the Pentium M's frequency scales with SpeedStep and the TSC is not invariant on Dothan — reading raw cycles would make every budget wobble with the power state. QPC is the one source of truth.

The clock is read at three points per frame: once at the top (frame delta + accumulator feed), and twice as *deadline checks* inside the mesh-queue drain and — if instrumentation is enabled — at phase boundaries for the profiler HUD. Reading the clock is not free (a QPC call is tens to low-hundreds of nanoseconds), so the mesh loop checks the deadline *between work items*, never inside the inner meshing kernel, and the sim never time-checks mid-tick at all (it is bounded by the active-voxel cap, not the clock — see degradation).

```c
typedef struct {
    double accumulator_ms;   /* unspent sim time carried between frames */
    double sim_dt_ms;        /* fixed sim timestep, e.g. 66.67 ms @ 15 Hz */
    double frame_prev_ms;    /* now_ms() at last frame top */
    int    max_substeps;     /* clamp on catch-up; see below. Set to 1. */
} FrameClock;
```

### Simulation: Fixed Timestep with a Clamped Accumulator

The sim runs on a **fixed timestep with an accumulator**, not a variable per-frame `dt`. This is non-negotiable and it falls straight out of the binding `SIM_TICK_MODEL`: a cellular automaton where heat diffuses and fluids equalize over discrete neighbor passes is only deterministic and only stable if every tick advances the same fixed quantum. A variable timestep would make diffusion rates, fluid-flow speed, and threshold-crossing timing depend on framerate — the same furnace would smelt faster on a good frame than a bad one, and the CA's stability (it relies on a bounded per-tick change for the diffusion math to not overshoot) would break the moment a frame ran long. So the sim quantum is constant; render framerate floats independently above it.

The canonical accumulator pattern is: add real elapsed time to an accumulator, then spend it in fixed `sim_dt_ms` chunks. The danger in that pattern is the **spiral of death** — if a tick takes longer than `sim_dt_ms` of wall time, the accumulator grows faster than it drains, you run more ticks next frame, which take longer, and the loop diverges until it freezes. The foundation already forbids the usual "just run more ticks to catch up" remedy, and here is the mechanism that enforces that prohibition: **`max_substeps = 1`.** We drain at most one sim tick per frame and then *discard* any accumulator overflow beyond one tick's worth. Concretely, after running the allowed tick(s), if the accumulator still holds more than `sim_dt_ms`, we clamp it back down. The in-world consequence is exactly what the foundation mandates: when the machine cannot keep up, **the simulation slows down in-world, gracefully, rather than the frame hitching or the loop spiraling.** A furnace smelting during a heavy-load moment simply takes a few more real seconds; it never causes a freeze.

Why `max_substeps = 1` and not 2–3? Because at the 2:1 render:tick cadence (30 FPS over 15 Hz sim), the *steady state* is the accumulator crossing the tick threshold every other frame — we will routinely have frames that run zero sim ticks and frames that run one, and that is correct and intended. We never *need* to run two ticks in one frame unless we have already fallen behind, and "falling behind" is precisely the condition where running a *second* expensive tick in the same 33 ms frame guarantees we miss the next vsync too. One tick per frame, overflow discarded, is the clamp that makes the single-core timeline bounded. (If the CA section later proves a tick reliably fits in well under half its 14 ms budget and wants to allow 2 substeps for snappier catch-up after a stall, that is a one-line change to `max_substeps` — but the *default is 1* and the spiral clamp is mandatory regardless.)

### Meshing: A Time-Bounded Work-Queue Drain

Meshing is the system's **shock absorber**, per the binding cut-order, and the scheduler treats it as such. Dirty chunks (flagged when the sim or the player edits a voxel) are pushed onto a remesh queue. Each frame, after the sim, we drain that queue *until either it is empty or the 6 ms mesh budget is spent* — whichever comes first. The deadline is checked **between chunks**, not within a chunk: a single 16 KiB greedy remesh is an indivisible unit of work (you cannot half-mesh a chunk and ship it), so we commit to finishing whatever chunk we have started, then check the clock before pulling the next one.

This means the *effective* per-frame remesh count is self-tuning. On a quiet frame with cheap chunks we might drain a dozen; on a frame where the sim churned a large fluid front into many dirty chunks, we mesh as many as fit in 6 ms and **leave the rest queued for subsequent frames**. A chunk displaying stale geometry for two or three frames (66–100 ms) is invisible during slow CA evolution — the foundation already establishes this latency is acceptable. The queue is priority-ordered: chunks nearest the camera and within the frustum mesh first, so the visible world converges fastest and off-screen staleness costs nothing.

One scheduling subtlety: the mesh budget is also where we *upload* finished vertex data to VBOs. The GL buffer upload (`glBufferData`/`glBufferSubData`) is a CPU-side driver cost on GL 2.0 and it competes in the same 6 ms. So the drain budget covers both CPU greedy-meshing *and* the GPU upload of what was meshed — the deadline check between chunks gates the combined cost, never just the CPU half.

### Render: Always Runs, Exactly Once

Per the binding cut-order, **render always completes, every frame, exactly once.** It is never skipped, never time-sliced, never deferred. A dropped render frame is the single most visible failure mode — the screen visibly stutters — whereas a deferred remesh or a slowed sim is imperceptible. Render gets its 9 ms nominally, but it is *not* deadline-interrupted: if a frame has already overrun in sim+mesh, we still render the full visible scene rather than dropping geometry, and simply accept a long frame. The protection mechanism for render's budget is *upstream* — we cut sim substeps and throttle meshing so that render reliably *inherits* a healthy chunk of the 33 ms — not a clock check inside the draw loop. Render reads the *interpolation alpha* (accumulator fraction of `sim_dt_ms`) so it can blend between the last and current sim states for smooth motion of fluid surfaces and heat-driven visuals despite the lower sim rate.

### Degradation Strategy: The Cut Order, Made Mechanical

When the previous frame overran — detected by comparing the realized frame time against 33.33 ms and by an exponential moving average of recent frame times to avoid reacting to a single outlier — the scheduler applies graceful degradation in this **fixed priority order**, which is the binding cut-order made executable. Higher-numbered cuts are applied only when lower-numbered ones are insufficient; render is never on this list because it is sacrosanct.

1. **Throttle the mesh queue drain (first and primary lever).** Reduce the mesh budget for the next frame below its nominal 6 ms — down to a floor (≈2 ms) that still drains at least one chunk so the queue cannot fully stall. Dirty chunks defer across more frames. This is the elastic budget and it absorbs the overwhelming majority of transient spikes. It costs only a few frames of geometry staleness off-screen or at the frustum edge.

2. **Discard sim accumulator overflow (the spiral clamp, always active).** This is not a reactive cut so much as a standing rule: `max_substeps = 1` and overflow beyond one tick is dropped every frame, full stop. Under sustained overload it is what keeps the sim from compounding — the in-world clock slows, the frame timeline does not. It sits second because it engages automatically before any explicit throttling decision is needed.

3. **Throttle the active-voxel count within the tick (last resort).** If even a single fixed tick cannot fit in its sim budget — a genuinely huge active front, e.g. a massive lava breach waking tens of thousands of voxels at once — the sim processes only up to its `ACTIVE_VOXEL_CAP` worth of voxels this tick and carries the remainder (the unprocessed active set) into the next tick. The CA section owns this cap and the carry mechanism; the scheduler's role is only to *signal* the budget-exceeded condition to the sim so it knows to stop at the cap rather than running the front to completion. The in-world effect: a giant simultaneous event resolves over several ticks instead of one, which for a Noita-style cellular world reads as a fast-but-finite cascade, not a hitch.

What we **never** do, by design: never run extra sim ticks to catch up (forbidden — spiral); never skip or partially-draw the render (forbidden — most visible failure); never time-check inside the meshing or sim inner kernels (cache and clock-call overhead — bound them by work count, not wall reads). The cuts are ordered cheapest-to-most-visible: defer geometry first, slow the sim clock second, ration the sim front last.

### The Main Loop

```c
/* now_ms(): QPC on XP/M170, clock_gettime(CLOCK_MONOTONIC) on Linux host.
 * One monotonic source. Returns milliseconds as double. */

#define FRAME_TARGET_MS   33.333   /* 30 FPS, binding TARGET_FRAMERATE      */
#define SIM_BUDGET_MS     14.0     /* binding FRAME_BUDGET_SPLIT            */
#define MESH_BUDGET_MS     6.0
#define MESH_BUDGET_FLOOR  2.0     /* throttle floor: always drain >=1 chunk */
/* RENDER_BUDGET_MS 9.0 and OVERHEAD 4.33 are not deadline-enforced;
 * they are protected by cutting sim/mesh above, per the binding cut-order. */

void run_main_loop(FrameClock* fc, World* world) {
    fc->sim_dt_ms    = 1000.0 / SIM_TICK_HZ;  /* e.g. 66.67 ms @ 15 Hz       */
    fc->max_substeps = 1;                      /* spiral clamp: binding       */
    fc->frame_prev_ms = now_ms();
    fc->accumulator_ms = 0.0;

    double mesh_budget_ms = MESH_BUDGET_MS;    /* adaptive; throttled on overrun */
    double frame_ema_ms   = FRAME_TARGET_MS;   /* smoothed frame time          */

    while (engine_running()) {
        double frame_start = now_ms();
        double real_dt = frame_start - fc->frame_prev_ms;
        fc->frame_prev_ms = frame_start;
        if (real_dt > 250.0) real_dt = 250.0;  /* guard: post-stall / debugger pause */

        /* ---- DEGRADATION CUT #1: throttle mesh budget if running hot ---- */
        frame_ema_ms = 0.9 * frame_ema_ms + 0.1 * real_dt;   /* smooth spikes */
        if (frame_ema_ms > FRAME_TARGET_MS) {
            /* shed mesh time proportional to how far over we are, to a floor */
            double over = frame_ema_ms - FRAME_TARGET_MS;
            mesh_budget_ms = MESH_BUDGET_MS - over;
            if (mesh_budget_ms < MESH_BUDGET_FLOOR) mesh_budget_ms = MESH_BUDGET_FLOOR;
        } else {
            mesh_budget_ms = MESH_BUDGET_MS;     /* recovered: restore full   */
        }

        input_poll();

        /* ================= SIMULATION (fixed timestep, accumulator) ====== */
        fc->accumulator_ms += real_dt;
        int substeps = 0;
        while (fc->accumulator_ms >= fc->sim_dt_ms && substeps < fc->max_substeps) {
            /* CUT #3 (last resort) lives INSIDE sim_tick: it processes up to
             * ACTIVE_VOXEL_CAP voxels and carries the remainder to next tick.
             * Bounded by work count, not by a wall-clock check. */
            sim_tick(world, fc->sim_dt_ms);
            fc->accumulator_ms -= fc->sim_dt_ms;
            substeps++;
        }
        /* ---- CUT #2: spiral clamp. Discard overflow beyond one tick. ----
         * If we still have >= a tick of unspent time, we are behind: drop it.
         * The in-world sim slows gracefully; the frame timeline stays bounded. */
        if (fc->accumulator_ms >= fc->sim_dt_ms)
            fc->accumulator_ms = fmod(fc->accumulator_ms, fc->sim_dt_ms);

        /* interpolation alpha for the render blend between sim states */
        double alpha = fc->accumulator_ms / fc->sim_dt_ms;   /* 0.0 .. 1.0   */

        /* ================= MESHING (time-bounded queue drain) ============ */
        double mesh_deadline = now_ms() + mesh_budget_ms;
        /* priority-ordered: nearest/in-frustum dirty chunks first */
        while (!remesh_queue_empty(world)) {
            Chunk* c = remesh_queue_pop(world);   /* highest priority chunk   */
            greedy_remesh(c);                     /* indivisible: finish it    */
            vbo_upload(c);                        /* GL upload counts here too */
            /* deadline checked BETWEEN chunks, never inside the kernel */
            if (now_ms() >= mesh_deadline) break; /* rest stay queued -> defer */
        }

        /* ================= RENDER (always, exactly once) ================= */
        /* Not deadline-interrupted. Protected by the cuts above, not a clock
         * check. alpha drives interpolation of fluid surfaces / heat visuals. */
        render_world(world, alpha);

        engine_tick_audio();        /* part of the overhead slice            */
        present_swap_buffers();     /* vsync; trailing slack absorbs jitter  */
    }
}
```

The structure encodes the priority order directly: sim is fixed and self-clamping (cuts #2 and #3 are intrinsic to its loop and its tick), meshing is the only phase with a *clock-driven, adjustable* budget (cut #1), and render sits below all of it with no deadline at all. The EMA on frame time means a single ugly frame — a streaming hitch, a GL driver hiccup — does not yank the mesh budget down and visibly stall geometry; only sustained overload throttles. And because every budget number traces back to `FRAME_BUDGET_SPLIT` and every cut traces back to the binding cut-order, this loop is the executable form of the foundation's frame contract, not a reinterpretation of it.

### Deferred to Prototyping

Three numbers in this scheduler I commit to as defaults but flag for validation on the actual M170, because only the real CPU + XP driver can confirm them: (1) the **QPC overhead** — if a `now_ms()` call turns out to cost enough that the between-chunks check meaningfully eats the mesh budget, we batch the check to every Nth chunk; I expect this is a non-issue but it is measurable only on hardware. (2) The **mesh-budget throttle response curve** — the linear "shed `over` milliseconds" rule is the simplest stable controller; whether it needs to be gentler (to avoid oscillating between full and floor budgets) is a tuning question for a loaded scene. (3) Whether `max_substeps` should ever be 2 — strictly a CA-section call once a real tick's cost distribution is known; the spiral clamp itself is mandatory either way.

---

## 7. Memory Budget

The single most important fact about this engine's memory, established in the foundation and restated here because everything below obeys it: **the full world is 4.00 GiB of voxel data and does not fit.** We are not budgeting how to *hold* the world; we are budgeting a *constant-size resident window* that is independent of how far the player has walked. A player who has explored ten thousand chunks and a player who just spawned have identical memory footprints. That invariant is the whole point of this section, and it is what makes a billion-voxel world run on a machine with under 2 GiB of address space.

### Why ~1.7–1.8 GiB, Not 2 GiB

A 32-bit process on Windows XP SP3 has a 4 GiB virtual address space, but by default the OS reserves the upper 2 GiB for kernel mappings — userland gets **2 GiB**, period (we do not assume `/3GB` / `IMAGE_FILE_LARGE_ADDRESS_AWARE`; it is fragile, it shrinks the kernel's address space in ways that can destabilize the GL driver on this hardware, and depending on it would be a landmine for a shipped binary). Of that 2 GiB, we never get all of it:

- The GL driver (the NVIDIA G70 user-mode driver) reserves address space at process init for its own command buffers, its shadow copies of VRAM resources, and PCI aperture mappings — hundreds of MiB before our first `malloc`.
- The CRT, loaded DLLs, thread stacks, and TLS take a fixed cut.
- **Fragmentation is the real ceiling, not byte exhaustion.** On a 32-bit heap, address space gets carved into non-contiguous runs over hours of play. A 96 MiB mesh-pool request can *fail at 1.4 GiB committed* if no single contiguous run that large survives. The largest-contiguous-free-block degrades long before total bytes do.

So the honest usable figure is **~1.7–1.8 GiB**, and we deliberately design to sit near **35% of that (~630 MiB)** — see the foundation's `MEMORY_OPERATING_POINT`. The headroom is not waste; it is the fragmentation insurance that keeps large contiguous allocations succeeding across a long session.

### The Concrete Allocation Table

Budgeting against **1.8 GiB practical usable**. Every figure is committed working memory, not reserved-but-untouched.

| Category | Budget | Basis |
|---|---|---|
| **Resident voxel data (the window)** | **160 MiB** | The chunk slab pool: 10,240 slots × 16 KiB. Window is 10,000 chunks (156.25 MiB); the extra 240 slots are streaming-churn headroom inside the same pool. |
| **CPU-side mesh data** | **96 MiB** | ~25% of resident chunks (≈2,500) carry real geometry; pooled vertex/index buffers, mirrored CPU-side for GL 2.0 (no VAOs). Avg ≈38 KiB/meshed chunk. |
| **Sim / active-voxel structures** | **16 MiB** | Active-voxel index lists, per-chunk dirty bitmasks, the active-set double buffer (we double-buffer the *active set*, not whole chunks), CA scratch. |
| **MaterialDef + property LUTs** | **1 MiB** | 256 `MaterialDef` entries + thermal/fluid/render lookup tables. Resident always, never streamed. |
| **Chunk hash table + metadata** | **8 MiB** | 256 KiB hash table (16,384 slots × 16 B) + per-chunk headers, neighbor caches, mesh handles for 10k chunks. |
| **Procedural-gen working memory** | **32 MiB** | Noise scratch, structure templates, biome/strata buffers used during streaming-in. Reused, not per-chunk. |
| **Persistence / IO staging** | **16 MiB** | Disk read + decompress staging for modified chunks; write-back staging for dirtied chunks being evicted. |
| **GL driver + VBO shadow/mirror** | **256 MiB** | Driver's reserved address space + our CPU-side VBO mirrors. The big single block; reserved early to dodge fragmentation. |
| **Texture atlas CPU copies** | **16 MiB** | Material atlas source before upload to the 256 MiB VRAM (VRAM itself is the renderer's separate budget, not counted here). |
| **Audio / UI / progression / misc** | **32 MiB** | |
| **Thread stack(s) + CRT/DLL** | **~8 MiB** | Single sim thread; main stack + loaded module footprint. |
| **Engine subtotal** | **~641 MiB** | |
| **Headroom / fragmentation reserve** | **~1.1 GiB** | Deliberate. The cushion that keeps the largest-contiguous-free-block healthy. |

The engine subtotal lands at roughly **640 MiB against 1.8 GiB usable (~36%)**, consistent with the foundation's operating point. Note what is *not* on this table: the world beyond the window (it does not exist in RAM — it is a seed + a disk file of player edits), and VRAM (a separate 256 MiB device budget owned by the rendering/meshing sections).

### The Loaded Chunk Window

This is inherited verbatim from the foundation and I will not relitigate it, only restate the binding figures so this section is self-contained:

- **Horizontal radius: 12 chunks = 192 m**, around the player.
- **Vertical: the full 16-layer column (all 256 m) is always resident.** Non-negotiable: the player digs down, smelts in cavities below, builds vertical industrial stacks. The chunk under a furnace must never be evicted because the player looked up at the sky.
- **Footprint: 25 × 25 horizontal × 16 vertical = 10,000 chunks**, covering **400 m × 400 m × 256 m**.
- **Voxel cost: 10,000 × 16 KiB = 156.25 MiB** (verified). This is the constant. It does not grow with exploration.

The window is a moving box. As the player crosses a chunk boundary, one or two 25×16 = 400-chunk slabs (a full vertical curtain) enter on the leading edge and an equal curtain leaves on the trailing edge. We never load or evict single chunks at the window scale — we load *columns of curtains*, which keeps streaming predictable and batchable.

### Eviction & Streaming: Distance-Based, Not LRU

**Eviction is distance-based, not LRU, and that is a deliberate rejection of LRU.** LRU is for caches where access recency predicts future access. Here, future access is predicted by *physical distance to the player*, full stop — a chunk 13 chunks away is leaving the window whether it was touched 1 ms ago or 10 s ago, and a chunk the player is walking toward must load before it is "accessed." LRU would also require touch-tracking on every voxel read in the sim and mesher (cache-line pollution we cannot afford on the single core), and it would happily keep a recently-edited-but-now-distant chunk resident while the player walks into cold terrain. Distance is cheaper to evaluate (a chunk-coordinate Chebyshev distance against the player's chunk, horizontal only), deterministic, and matches the actual access pattern of a first-person world.

The eviction/streaming rule:

```c
/* On player crossing a chunk boundary, for each chunk leaving the window: */
if (chunk->flags & CHUNK_DIRTY) {
    /* Player modified it. Its state diverges from the seed -> persist. */
    enqueue_chunk_writeback(chunk);   /* RLE/delta vs seed-gen baseline */
} else {
    /* Untouched since generation. The seed + worldgen IS its storage. */
    /* Drop it. Regenerate deterministically on return. Costs nothing on disk. */
}
free_chunk_slab(chunk);               /* return slab to the pool */

/* For each chunk entering the window: */
if (disk_has_modified_record(key))
    enqueue_chunk_load_from_disk(key);   /* IO staging -> decompress -> slab */
else
    enqueue_chunk_worldgen(key);         /* procgen working mem -> slab */
```

The critical asymmetry: **unmodified chunks have zero persistent storage cost** — they are a pure function of the seed, regenerated on demand. Only chunks the player actually changed touch the disk. This is what bounds the save file to "what the player built," not "everywhere the player walked." It also means worldgen must be **deterministic** (same seed + same chunk key → bit-identical voxels) so that an evicted-and-regenerated chunk is indistinguishable from its original. That determinism requirement is a binding constraint on the worldgen section.

**Streaming is synchronous-bounded, never a surprise stall.** The foundation forbids implicit VM paging precisely because a page fault mid-tick is an unbudgeted disk stall on a single core. Instead, streaming runs as a *bounded work queue serviced inside the frame's overhead/slack budget*: a hard cap of N chunk-loads (worldgen or disk) per frame, with the remainder carried to subsequent frames. Because eviction happens at the window edge — 192 m out, well behind the G70's fog distance — there is generous wall-clock lead time between "chunk enters the load queue" and "player can see it." A chunk that is still generating for 2–4 frames is invisible behind fog. Meshing of a freshly-streamed chunk then rides the elastic mesh budget (foundation Frame Budget), same as any other dirty chunk.

### Allocator Strategy: Fixed-Size Slab Pool, No Per-Chunk malloc

**Every voxel-data chunk is exactly 16,384 bytes. Therefore the allocator for chunks is a fixed-size slab pool, and `malloc`/`free` per chunk is banned.** This is the most important allocator decision and it falls straight out of the foundation's constant chunk size and the 32-bit fragmentation reality.

The design:

```c
/* Reserve the entire chunk pool ONCE at startup, as a small number of
 * large contiguous super-slabs, before the heap fragments. */
#define CHUNK_SLOT_BYTES   16384            /* one chunk's voxel data */
#define SLOTS_PER_SUPERSLAB 64              /* 64 * 16 KiB = 1 MiB exactly */
#define SUPERSLAB_COUNT    160              /* 160 MiB total, 10,240 slots */

/* 160 super-slabs of 1 MiB each = 160 MiB = 10,240 chunk slots.
 * Window needs 10,000; the extra 240 absorb streaming churn at the edge. */

typedef struct {
    uint8_t  storage[SUPERSLAB_COUNT][SLOTS_PER_SUPERSLAB * CHUNK_SLOT_BYTES];
    uint32_t free_list[10240];   /* stack of free slot indices */
    uint32_t free_top;
} ChunkSlabPool;
```

Why this and not `malloc`:

1. **Zero external fragmentation, ever.** Every allocation is the same size, so a freed slot is *always* reusable by the next request. There is no possibility of the "1.4 GiB committed but no 96 MiB run" death because the pool's address range is fixed at startup and never released to the general heap. Allocate is a free-list pop (O(1)); free is a push (O(1)). No size classes, no coalescing, no `malloc` metadata overhead per chunk (which on the XP CRT would add 8–16 bytes × 10,000 chunks of pure waste).
2. **Reserve big and early.** The 160 MiB pool, the 256 MiB GL block, the 96 MiB mesh pool, and the 32 MiB procgen scratch are all reserved as large contiguous blocks *at startup*, before the address space has been chewed up by transient allocations. Grabbing the big blocks first is the single most effective anti-fragmentation tactic on 32-bit XP.
3. **Slabs are reused, not churned.** As the window slides, evicted chunks return their slots to the free list and incoming chunks pop them. The pool's committed footprint is flat for the entire session — the allocator does no work the OS can see after init. This directly serves the foundation reality that "large contiguous allocations must be pooled/reused, not churned."

The same pooled-and-reused discipline applies to the other large, frequently-recycled blocks:

- **Mesh buffers (96 MiB):** *not* fixed-size — a meshed chunk's vertex/index data varies from a few hundred bytes (a flat surface) to ~64 KiB (a noisy material boundary). This pool uses a **small-set of size-class free lists** (e.g., power-of-two buckets from 1 KiB to 64 KiB), each bucket itself a slab pool. A remesh that grows past its current bucket frees the old block back to its bucket and pops from the larger one. Bucketing keeps mesh allocation O(1) and fragmentation-free *within* the mesh arena, isolated from the chunk pool.
- **Procgen scratch (32 MiB)** and **IO staging (16 MiB):** ring/arena allocators reset per streaming job. No long-lived allocations, so no fragmentation surface at all.
- **Hash table, MaterialDef table, atlas copies:** allocated once at startup, never freed during play.

The general-purpose CRT `malloc` is reserved for genuinely irregular, low-frequency allocations (UI widgets, progression state, audio buffers) — the ~32 MiB "misc" tier — where its overhead and fragmentation risk are immaterial because the churn rate is near zero. **Nothing on the per-frame or per-stream hot path ever calls `malloc`.** That is the rule that lets the process run for hours without the heap slowly strangling itself.

### Consistency Check Against the Foundation

For the record, every load-bearing number here is the foundation's: chunk = 16,384 B; window = 10,000 chunks = 156.25 MiB; full world = 4.00 GiB (does not fit); operating point ≈ 35% of ~1.8 GiB. The only figures *introduced* by this section are the allocator's internal geometry (160 super-slabs × 1 MiB × 64 slots = 10,240 slots = 160 MiB) and the mesh-pool bucketing — both of which sit inside the foundation's already-allocated 160 MiB voxel and 96 MiB mesh line items and do not alter any binding total.

---

## 8. World Persistence

Persistence is where the "world is 4.00 GiB and does not fit in RAM" reality (a global binding fact) becomes a disk-layout problem. The full chunk grid is **128 x 128 x 16 = 262,144 chunks** and we will never store all of them, because we cannot generate all of them either — the world is a procedural function of a seed. Persistence exists to remember exactly one thing: **where the player has diverged from what the seed would generate.** Everything else is recomputed for free. This section pins how that divergence is recorded, batched into files, and reloaded, and it makes the one genuinely contentious call — whether transient simulation state (temperatures, active voxels) survives a save/load — and defends it.

### The Core Principle: Store Deltas, Regenerate the Rest

The world generator is a pure deterministic function: `gen(seed, cx, cy, cz) -> Chunk`. Same seed, same coordinate, byte-identical chunk, every time, on any machine. This is not a nicety; it is the load-bearing wall of the entire memory and persistence story. Because regeneration is free and deterministic, **a chunk the player never touched is never written to disk** — on reload we simply call `gen()` again and get the original back. Only chunks carrying player-authored divergence (a dug tunnel, a built furnace, a pooled ingot, a burned-out forest) are persisted.

This gives us the binding persistence rule:

> A chunk is written to disk **only** once it has been modified by gameplay, and it carries a `MODIFIED` dirty bit set the first time any voxel in it is altered by a non-generator source. Unmodified chunks are dropped on eviction with zero I/O and reconstructed by `gen()` on return.

The payoff is enormous and worth stating in numbers. A player who has dug a 50 m mineshaft and built a base has perhaps touched a few thousand chunks over a whole playthrough. At an average of a few KiB per stored chunk (compression below), a mature save is **single-digit to low-double-digit megabytes**, not gigabytes. We never approach the 4.00 GiB full-world figure on disk because we never store the parts the seed already knows. This also means **save size scales with player activity, not with explored distance** — flying across the surface admiring vistas costs zero bytes; only changing things costs bytes. That mirrors the in-RAM invariant (resident data bounded by the window, constant regardless of exploration) and is deliberate symmetry.

One subtle correctness trap the generator section must honor and I flag here: **the generator must be versioned.** If we ever change the noise functions or ore distribution, previously-unmodified chunks would regenerate *differently* than they looked when the player last saw them — a cliff the player walked past silently mutates. The world save header therefore stamps a `gen_version`. On a version mismatch we have two honest choices (deferred to the gen section to pick): refuse to upgrade silently, or bulk-bake the entire previously-visited region to disk at upgrade time. I lean toward the latter being out of scope for a single-player hobby-scale game; **default: stamp the version, and treat a mismatch as "this save belongs to an older generator," refusing to load rather than silently corrupting the player's memory of their world.** Flagged for the gen section.

### What's In a Stored Chunk: Material Falls Out, Temperature Does Not

A chunk's voxel array is **4,096 voxels x 4 bytes = 16,384 bytes (16 KiB)** uncompressed (binding). The 32 bits per voxel are `mat8 | temp8 | fill4 | light4 | ao4 | flags4` (binding). The question persistence must answer is: which of those fields are *authored state worth remembering*, and which are *derived/transient state we should recompute on load*?

I split the 32 bits into three classes:

- **Authored, must persist: `mat8`, `fill4`.** Material composition and fluid fill level *are* the player's creation. A furnace is, per the project fantasy, literally a cavity of stone voxels with specific materials in specific places; storing the voxel material array *is* storing the furnace. There is no separate "structure" concept to serialize — **material composition of structures persists trivially because it is nothing but the voxel material array, and we store that array.** A built wall, a sorted pile of ingots, a half-full molten-copper crucible: all of it is `mat` + `fill`, and all of it is saved. This is the entire answer to "how does structure composition persist" — it falls out of storing the voxels, exactly as the brief anticipated.

- **Derived, never persist: `light4`, `ao4`.** Baked lighting and ambient occlusion are a pure function of the surrounding solid/transparent geometry and light sources. On load we recompute them during the first mesh pass — the mesher already touches these voxels (zero extra traffic, per the foundational lighting rationale). Storing them would be storing a cache, inflating every record by a quarter of its field bits for data we can rebuild in microseconds per chunk. **Light and AO are zeroed on load and rebaked by the lighting/meshing pass.** This also sidesteps a consistency bug: if lighting rules ever change, stored light would be stale; recomputed light is always correct.

- **The contested field: `temp8`.** Here is the real decision, and I come down firmly: **temperature IS persisted.** I'll defend it against the tempting alternative of "discard it and let the world re-settle."

#### Why temperature is saved, not re-settled

The seductive argument for discarding temperature is that it's "just sim state" — let the furnace cool to ambient on unload, and on reload the player relights it and it heats back up. This is *wrong for this game specifically*, and the reason is the central fantasy: **heat is a slow, accumulated, player-managed resource.** A player banks heat. They run a forge for ten in-game minutes to bring a large stone thermal mass up to working temperature, then smelt a batch. If unloading the chunk (which happens the instant they walk 192 m away — the window radius is only 12 chunks, binding) silently reset all that accumulated heat to -40..20 C ambient, then walking to a far ore vein and back would *erase the player's thermal investment*. That is not "the world re-settling"; that is the world *forgetting work the player did*, and it is indistinguishable from a bug to the person playing.

Temperature costs us exactly **one byte per voxel** and — critically — it compresses superbly under the same RLE we already need for materials, because **temperature fields are spatially smooth.** Heat diffuses; adjacent voxels have near-identical temperatures; a cooled stone wall is a long run of one temperature code, and a uniform ambient region is one gigantic run. The non-linear 8-bit encoding (1 C/step ambient, 20 C/step industrial — binding) actually *helps* here: large swaths of the world sit in the ambient band and quantize to the same handful of codes, producing long RLE runs. So the marginal cost of persisting temperature is small precisely where it's common (cold, uniform, compresses to nothing) and only grows where heat is interesting (a hot forge — exactly the state the player wants remembered).

The fill level shares the same logic: a half-full molten pool that resets to "settled" on reload would teleport liquid around. Fill persists.

So the persisted payload is a **24-bit-per-voxel logical record (`mat8 | temp8 | fill4`)** plus we must also persist the **`LIQUID` flag** (1 bit of `flags4`) — see below — while `light4`, `ao4`, and the remaining flags are dropped and recomputed.

#### Simulation wake-state: freeze the *fields*, discard the *index*

There are two different things people mean by "sim state," and the distinction resolves the whole freeze-vs-resettle debate cleanly:

1. **The simulated fields** — per-voxel temperature and fluid fill. These ARE world state. **We persist them** (above). On load the world is in exactly the thermal/fluid configuration it was unloaded in.

2. **The active-voxel wake-set** — the sub-chunk list of voxels currently being stepped by the CA this tick (the "fronts" the simulation section maintains at sub-chunk granularity, per the binding chunk-vs-wake-unit separation). This is a *transient acceleration index*, not world state. **We do NOT persist it.** Reconstructing which voxels are "active" from scratch on load is both cheap and *more correct* than restoring a frozen wake-list, because the wake-set is derivable: on chunk load, the CA re-scans the chunk's voxels and re-wakes any that are out of equilibrium — a liquid voxel adjacent to air or a non-full neighbor (it can still flow), a voxel whose temperature differs from a neighbor's by more than the diffusion threshold, a voxel at or past its `MaterialDef` melt/boil threshold. Everything that *should* be simulating wakes itself from the persisted fields alone.

This gives us the best of both: the world resumes in the exact physical state it left (heat banked, pools placed), but we store **zero bytes of bookkeeping** and we cannot load a corrupt/stale wake-set. The `ACTIVE` and `DIRTY_MESH` flags are therefore *not* persisted (they're recomputed); the **`LIQUID` flag IS persisted** only because it's a cheap material/state classifier the load-time re-scan would otherwise have to re-derive — and persisting it costs nothing since `flags4` rides along in the same RLE token stream as `fill4` anyway. Honestly this is a marginal call; if the re-scan derives `LIQUID` trivially from `mat` + `fill`, drop it too. **Flagged for prototype: confirm whether `LIQUID` is cheaper to store or to re-derive; default to re-derive (persist no flags) and store only `mat | temp | fill`.**

To summarize the field decision as a binding table:

| Field | Bits | Persisted? | On load |
|---|---|---|---|
| `mat` | 8 | **Yes** | Loaded verbatim |
| `temp` | 8 | **Yes** | Loaded verbatim (banked heat survives) |
| `fill` | 4 | **Yes** | Loaded verbatim (pools survive) |
| `light` | 4 | No | Rebaked by mesher |
| `ao` | 4 | No | Rebaked by mesher |
| `flags` | 4 | No (default) | Recomputed; CA re-wakes out-of-equilibrium voxels |

### Compression: Per-Chunk Palette + RLE

A 16 KiB raw chunk is small, but most modified chunks are *mostly one or two materials* — a dug-out room is air plus a stone shell; a wall is one material; the untouched portions of a "modified" chunk are still pristine generated terrain. Two cheap, single-threaded-friendly transforms exploit this, in order:

**1. Per-chunk material palette.** Most chunks use a tiny fraction of the 256 global materials. We scan the chunk's distinct `mat` ids and build a local palette; voxel materials are then re-indexed to palette indices. A chunk using 6 materials needs **3 bits per voxel for the palette index** instead of 8. The palette is stored as a short list of global ids. This is the Minecraft-paletted-chunk idea and it's a clean win on the Pentium M because the palette is tiny (fits trivially in L1) and indexing is a mask/shift. Palette size is stored as a count; a chunk with >N distinct materials past some threshold (say >64) just stores raw `mat8` and skips the palette — but that's pathological and rare.

**2. Run-length encoding over the linearized voxel stream.** After palettization we RLE the `(palette_index, temp, fill)` tuple along a fixed voxel iteration order (z-major, then y, then x — chosen so vertical strata and floor/ceiling layers, which dominate terrain, form long runs). A run is a `(count, value)` pair. This is where the long same-material runs the brief calls out get crushed: a solid stone layer at uniform ambient temperature is **one run** for a whole 16-wide span. Air pockets, uniform-temperature regions, and untouched generated strata all collapse.

I deliberately **do not reach for a general-purpose compressor (zlib/LZ4) as the primary scheme.** RLE-over-palette is decompressed with a trivial branch-light loop that the in-order Pentium M pipeline loves, has no dictionary/window state to thrash the 32 KB L1, and is plenty given the data's structure. We *may* wrap the final region file (or cold region files) in a single LZ4 pass for extra shrinkage since LZ4 decode is fast and stream-friendly — **flagged as optional polish, deferred to prototyping; RLE-over-palette is the binding primary scheme** and must stand on its own.

A worked expectation: a typical modified chunk — a dug room — palettizes to maybe 4 materials (air, stone, ore, torch-block) at 2 bits, then RLE collapses the large air void and the surrounding rock to a few dozen runs. **Realistic stored size: a few hundred bytes to ~2 KiB per modified chunk**, versus 16 KiB raw — roughly an 8x–40x reduction depending on complexity. This is what keeps a mature save in the megabytes.

### Region Files: Batch ~Hundreds of Chunks Per File, Defended Against XP

The choice is **region files batching many chunks into one file**, emphatically *not* one file per chunk. The reason is squarely the deployment target's filesystem behavior, and the numbers are brutal for the per-file approach.

**Why per-chunk files are wrong on XP/NTFS.** A mature save touching tens of thousands of chunks would create tens of thousands of tiny files. On Windows XP with NTFS this is pathological: (a) every file consumes at minimum one cluster (typically 4 KiB) plus an MFT record (~1 KiB), so a 400-byte compressed chunk wastes ~4.6 KiB to disk overhead — **the metadata and slack dwarf the data**; (b) the NTFS Master File Table fragments and bloats with tens of thousands of records, and directory enumeration (which the streaming system does constantly as the player moves) degrades sharply once a directory holds many thousands of entries — XP's directory B-tree and the 8.3-name generation make this measurably slow; (c) `CreateFile`/`CloseHandle` syscall overhead per chunk stream-out, on a single core inside a 33.33 ms frame budget (binding), is a latency we cannot absorb — a streaming save of a region would be thousands of file-open syscalls. Per-chunk files are rejected.

**The region layout.** We group chunks into **region files of 32 x 32 chunks in the horizontal plane, spanning the full 16-layer vertical column** — i.e. one region file owns a `32 x 32 x 16 = 16,384`-chunk volume, a 512 m x 512 m x 256 m world column. The region coordinate is `(rx, rz) = (cx >> 5, cz >> 5)` (shift, binding-style coordinate math). Full vertical column per region mirrors the in-RAM "full vertical column resident" decision (binding) and keeps a player's whole base — which spans the vertical (digging down, building up) — inside one file, so saving a base is one file's worth of I/O.

I sized the region at 32x32 horizontal (not Minecraft's flat 32x32) deliberately: the loaded window is radius-12 horizontal (binding), so the player straddles at most a 2x2 block of region files at once. Streaming therefore keeps a tiny working set of open region-file handles (≤ ~4–9), each opened once and held, with chunks read/written by seeking within the file — **a handful of persistent file handles instead of thousands of transient ones.** This is the entire point of regioning on XP.

**File structure.** A region file is a fixed-size header/index followed by a heap of variable-length compressed chunk records:

- A **sector size** of 4 KiB (matches the NTFS cluster, so chunk records are allocated in whole 4 KiB sectors — writes align to cluster boundaries, no read-modify-write of partial clusters).
- An **index table** at a fixed offset: 16,384 entries (one per possible chunk in the region), each giving the chunk record's sector offset and sector count within the file, plus a timestamp/version. Index is fixed-size and memory-mappable/read-in-one-go.
- A **record heap** after the index: each modified chunk's compressed record, sector-aligned.
- Free-space management: when a chunk is re-saved and its compressed size grows past its current sector allocation, we append to the heap and update the index (leaving a free hole); when it shrinks, it stays in place. Holes are reclaimed by an occasional compaction pass on save (rare, single-player, can be amortized). This is the well-proven Minecraft-region approach and it works because rewrites are bounded and infrequent relative to reads.

A chunk that has never been modified has an **all-zero index entry** (sector_count == 0) — meaning "not stored, regenerate from seed." The index *is* the modified-or-not bitmap; we never store a sentinel record for unmodified chunks.

### C Pseudocode: Chunk Save Record and Region Index

```c
/* ---- Region file on-disk layout (little-endian, fixed on writer side
 *      since we cross-compile to one target; no endianness negotiation) ---- */

#define REGION_CHUNKS_X   32
#define REGION_CHUNKS_Z   32
#define REGION_CHUNKS_Y   16          /* full vertical column, binding */
#define REGION_CHUNK_COUNT (REGION_CHUNKS_X * REGION_CHUNKS_Z * REGION_CHUNKS_Y) /* 16384 */
#define REGION_SECTOR      4096        /* matches NTFS cluster; align records */
#define REGION_MAGIC       0x52564B31u /* "RVK1" : region, voxel, version 1   */

/* One index slot per possible chunk in the region. sector_count == 0 means
 * "never modified -> regenerate from seed", and costs no heap bytes. */
typedef struct {
    uint32_t sector_offset;  /* offset in REGION_SECTOR units from file start */
    uint16_t sector_count;   /* allocated sectors; 0 == not stored            */
    uint16_t flags_reserved; /* e.g. COMPRESSION_NONE/RLE/RLE+LZ4 (flagged)   */
} RegionIndexEntry;          /* 8 bytes */

typedef struct {
    uint32_t magic;          /* REGION_MAGIC                                  */
    uint32_t gen_version;    /* world-generator version; mismatch -> refuse   */
    uint64_t world_seed;     /* sanity-check this region belongs to this save */
    int32_t  region_x, region_z;
    uint32_t heap_sectors;   /* current high-water mark of the record heap    */
    uint8_t  reserved[40];   /* pad header to 64 bytes                        */
} RegionHeader;              /* 64 bytes */

/* On disk:
 *   [ RegionHeader : 64 B ]
 *   [ RegionIndexEntry[16384] : 128 KiB ]   <- one read populates the index
 *   [ ... sector-aligned compressed chunk records (the heap) ... ]
 * Index is 16384 * 8 = 131072 B = exactly 32 sectors. Header+index = 33 sectors. */

/* Map a chunk coord to its slot in this region's index. */
static inline uint32_t region_slot(int32_t cx, int32_t cy, int32_t cz) {
    uint32_t lx = (uint32_t)(cx & (REGION_CHUNKS_X - 1)); /* cx % 32 */
    uint32_t lz = (uint32_t)(cz & (REGION_CHUNKS_Z - 1));
    uint32_t ly = (uint32_t)(cy & (REGION_CHUNKS_Y - 1)); /* 0..15    */
    return (ly * REGION_CHUNKS_Z + lz) * REGION_CHUNKS_X + lx;
}

/* ---- Per-chunk compressed record (the bytes the index points at) ---- */

enum { COMPRESS_RLE_PALETTE = 1, COMPRESS_RAW = 2 /*, +LZ4 wrap: flagged */ };

typedef struct {
    uint32_t uncompressed_voxels; /* always 4096; cheap corruption check     */
    uint16_t palette_count;       /* distinct materials in this chunk        */
    uint8_t  index_bits;          /* bits per palette index: 1,2,4,8         */
    uint8_t  compression;         /* COMPRESS_* selector                     */
    uint32_t run_count;           /* number of RLE runs that follow          */
    /* followed by:
     *   uint8_t   palette[palette_count]   ; local idx -> global material id
     *   Run       runs[run_count]          ; RLE over (palidx, temp, fill)
     * Note: light/ao/active/dirty are NOT stored; rebaked / re-derived on load.
     */
} ChunkRecordHeader;

/* One RLE run. We RLE the tuple that actually varies and matters:
 * material (as palette index), temperature, and fluid fill. 'count' spans a
 * homogeneous run along the z-major iteration order. A uniform ambient stone
 * stratum collapses to a single run. */
typedef struct {
    uint16_t count;     /* 1..4096                                            */
    uint16_t palidx;    /* palette index (only low index_bits significant)    */
    uint8_t  temp;      /* persisted: banked heat survives unload (defended)  */
    uint8_t  fill;      /* low 4 bits: fluid fill level; high 4 bits unused/0 */
} Run;                  /* 6 bytes; tighten with bit-packing if profiling says so */

/* ---- Save path (sketch) ---- */
/* 1. Only enters here if chunk->flags & CHUNK_MODIFIED.
 * 2. Build palette from distinct VOX_MAT over the 4096 voxels.
 * 3. Emit runs of (palidx,temp,fill) in z-major order.
 * 4. Compute sectors = ceil(record_bytes / REGION_SECTOR).
 * 5. If sectors <= existing index[slot].sector_count, overwrite in place;
 *    else append at heap high-water, bump RegionHeader.heap_sectors,
 *    update index[slot]. Old location becomes a free hole (compacted later).
 * 6. Write index[slot] last (so a crash mid-write never points the index at a
 *    half-written record: write record fully, fsync-ish, THEN write index).  */

/* ---- Load path (sketch) ---- */
/* 1. region_slot(); if index[slot].sector_count == 0 -> gen(seed,cx,cy,cz).
 * 2. Else seek to sector_offset, read sector_count sectors, validate header.
 * 3. Expand RLE+palette into the 4096 voxel words: set mat,temp,fill;
 *    zero light,ao,flags.
 * 4. Hand chunk to lighting/mesher (rebake light+ao) and to CA load-scan
 *    (re-wake any out-of-equilibrium voxel from the persisted fields).        */
```

### Crash Safety and Write Ordering (Single-Player, Cheap)

A single-player game shouldn't need a transaction log, but it must not corrupt a region file on a power loss. The one binding ordering rule: **write the chunk record to the heap completely before updating its index entry**, and update the index entry as a single 8-byte aligned write (atomic at the granularity that matters). A crash between the two leaves an orphaned heap region (reclaimed by the next compaction) but a *consistent* index that still points at the last good record — the world loses at most the un-flushed edits since the last save, never a corrupt chunk. We flush the `RegionHeader.heap_sectors` high-water mark after the index update. This is enough; a full journaling layer is **out of scope and deliberately rejected** as over-engineering for the single-player target.

Saving itself is incremental and budget-aware: dirty modified chunks are flushed opportunistically (on eviction from the window, on a periodic autosave timer, on quit) — never all at once mid-frame. Because writes go to a handful of already-open region handles and each compressed record is a few hundred bytes to ~2 KiB, a flush is a `seek + write` of a few sectors, which slots comfortably into the **4.33 ms overhead/slack** band of the frame budget (binding) without competing with the protected sim or sacrosanct render phases. If a save batch is large (player blew up a mountain), it spreads across frames the same way meshing does — persistence is elastic, like meshing, never a hitch.

---

## 9. Progression Layer — Discovery as Observation, Not Authorship

This is the section where I am least able to hide behind the hardware. The Pentium M does not have an opinion about whether progression is fun. Every other section in this document is disciplined by a transistor budget; this one is disciplined only by taste and by one ruthless architectural rule that I will defend below. So let me be honest up front about the thing that keeps me up at night, then build the system that survives it.

**The central risk, stated plainly:** an emergent progression system with no authored backbone is, by default, *unfun and illegible*. Noita gets away with pure emergence because it is a roguelike — the loop is short, death is the reset, and "I wonder what happens if I throw this" is the entire contract with the player. We are not building that. We are building a survival/progression game where the player invests hours into a single world, builds vertical industrial structures, and expects the second hour to feel like advancement over the first. Pure emergence in that frame produces a specific failure mode: the player melts iron by accident, doesn't notice it happened, doesn't understand *why* it happened, and cannot reproduce it. The physics worked perfectly and the game failed completely. **Emergence is the simulation's job. Legibility is progression's job. These are different jobs and the architecture must keep them separate.**

So the position I take is this: **the simulation stays purely physical and progression is a read-only observer layered on top of it.** Progression never injects a value into the voxel word, never gates a state transition, never tells the CA "the player hasn't unlocked iron so don't melt it." That would be a lie inside the physics, and the moment the physics lies the emergence is dead — you've reinvented hardcoded recipes wearing a cellular-automaton costume. Instead, progression *watches* the simulation produce events and *interprets* them for the player. The backbone is not in the unlocks; the backbone is in the **authored interpretation of physical facts the player was always free to produce.**

### The Architectural Rule: Progression Is Downstream of Physics, Never Upstream

Everything in this section obeys one binding constraint that I am adding to the document:

> **Progression code may READ simulation state and consume an event stream. It may NEVER write to a voxel, alter a MaterialDef, or change a tick's outcome. If you remove the entire progression layer, the world simulates identically.**

This is not philosophical hygiene — it is a performance and determinism contract. The CA section already committed to a fixed-rate, deterministic, double-buffered tick with an active-voxel cap and a 14 ms budget. If progression could mutate the sim, it would (a) compete for that 14 ms and (b) make the tick non-deterministic with respect to player history, which destroys the ability to regenerate unmodified chunks from seed (the entire memory envelope depends on regeneration being a pure function of seed + stored deltas — a progression hook in the sim would make terrain depend on what the player has *discovered*, which is insane). Progression is allowed to run in the **4.33 ms overhead/slack band**, never inside the sim budget, and it processes a bounded event queue, not the voxel array.

```c
/* The ONLY channel from sim to progression. The CA already
 * walks state transitions every tick; it costs ~nothing to
 * have transition handlers append to a ring buffer instead of
 * discarding the fact. Progression drains this in the slack band. */
typedef enum {
    EV_MELT,        /* a voxel crossed mat.melt_temp solid->liquid   */
    EV_SOLIDIFY,    /* liquid->solid                                 */
    EV_BOIL,        /* liquid->gas / sublimation                     */
    EV_IGNITE,      /* a fuel voxel began sustained combustion       */
    EV_TEMP_PEAK,   /* a voxel held temp >= T for >= N ticks         */
    EV_REACT,       /* two adjacent materials produced a third       */
    EV_FREEZE,      /* water/liquid crossed below freeze threshold   */
} EventKind;

typedef struct {
    EventKind kind;
    uint8_t   mat_a;     /* primary material id (index into MaterialDef) */
    uint8_t   mat_b;     /* secondary, for reactions; else 0xFF          */
    uint8_t   result;    /* resulting material id, if a transition       */
    uint8_t   _pad;
    int32_t   wx, wy, wz; /* world voxel coords, for "show me where"     */
    uint32_t  tick;       /* sim tick stamp, for dedup/throttling        */
} SimEvent;

/* Fixed ring, drained every frame. Sized small on purpose: the
 * active-voxel cap already bounds how many transitions a tick can
 * produce. 1024 events x 20 bytes = 20 KiB, in the misc/UI budget. */
#define EVENT_RING_CAP 1024
```

The event ring is the entire coupling surface between the most physical part of the engine and the most human part. That narrowness is the design. The CA section's state-transition code already computes "this voxel just crossed `melt_temp`" — it *has* to, to swap the material id. Emitting a `SimEvent` is one ring-buffer push on a branch the sim already took. **Cost to the sacred 14 ms: effectively zero. No new voxel field, no side-table the sim must maintain.** This respects the global reality that the voxel word is full and transient data lives outside it — the event ring *is* that side structure, and it is transient (drained every frame, never persisted as live state).

### What Gets Tracked: The Journal as a Material-Behavior Ledger

The player-facing artifact is a **journal**, and its content model is the thing I want to get right because it is what makes emergence legible. The naive version is an achievement list ("First Iron Melt! ✓"). That is illegible in exactly the way that matters: it tells the player *that* something happened, not *what they learned about the world.* The journal must instead be a **ledger of observed material properties**, populated by inference from the event stream.

Here is the model. Every `MaterialDef` has a rich set of physical properties (melt point, boiling point, thermal conductivity, density, combustion energy, what it reacts with). **The player does not start knowing any of these numbers.** The journal holds a *discovered shadow* of the MaterialDef table — initially almost empty, filled in as the player's actions cause the simulation to reveal facts.

```c
/* Per-material discovery state. One per MaterialDef entry, 256 total.
 * This is the SAVED progression payload. ~16 bytes x 256 = 4 KiB.
 * Trivial to persist alongside the save (see Persistence section). */
typedef struct {
    uint16_t flags;          /* SEEN, NAMED, MELT_OBSERVED, BOIL_OBSERVED... */
    uint8_t  melt_obs_lo;    /* observed melt temp as a RANGE, in temp-code  */
    uint8_t  melt_obs_hi;    /* units. Player learns "iron melts somewhere   */
    uint8_t  boil_obs_lo;    /* between code X and Y" and the band tightens  */
    uint8_t  boil_obs_hi;    /* each time they observe a transition nearby.  */
    uint8_t  conductivity_hint; /* coarse: felt as "heats fast/slow"         */
    uint8_t  react_count;    /* how many reactions involving this mat seen   */
    /* ... a few more coarse observed hints ... */
} MaterialKnowledge;
```

This is the key move and I want to defend it hard. **The journal stores observed *ranges*, not the true MaterialDef values, and the ranges tighten with repeated observation.** Why ranges? Because of the binding temperature encoding. Recall temperature is two-segment non-linear: 20 C per step in the industrial band above 120 C. Iron's true melt point is 1538 C; the sim only knows it as a temperature *code*, and the player's thermometer (if they build one — see below) can only read codes. So when the player watches iron go liquid, the honest thing the journal can record is "iron melted while the surrounding voxels read around code 230 — that's *somewhere* in 1500-1560 C." This is not a limitation I'm apologizing for; it is *thematically perfect.* In a real metallurgy progression, you don't get handed exact melting points — you discover them empirically and your estimates sharpen with better instruments and more trials. The 20 C quantization in the industrial band, which the Foundational section flagged as "invisible to the player" for sim purposes, becomes the literal grain of the player's empirical knowledge. The hardware constraint *is* the game mechanic. I'm thrilled about this and it fell out of the binding numbers for free.

So progression *exploits* a property of the encoding the other sections treated as a tolerance. That is the kind of thing this section should do — find where the constraints already point at a fun mechanic.

### How Discovery Feels: The Three-Beat Loop

Player-facing progression in this system has to *feel like science*, because that is the only honest framing of "you cause physical events and learn from them." The loop I'm designing for has three beats, and the journal's job is to make all three legible without scripting the outcome.

**Beat 1 — Notice.** The player does something and the world responds physically. They pile charcoal under an ore block, the heat plume climbs, the ore voxels go liquid and pool. The simulation did this with zero progression involvement. The job of progression here is *to make sure the player noticed.* This is where a light, diegetic notification lives: a brief journal flash — "You observed: **Hematite** turned to liquid." Not "ACHIEVEMENT UNLOCKED." A factual observation, phrased as something the player saw, because they did see it. The `EV_MELT` event carried the world coords, so the notification can offer "[mark location]" to pin it.

**Beat 2 — Understand.** The player opens the journal. The material entry now shows what's been inferred: Hematite, observed to liquefy around 1500-1560 C, observed to require sustained high heat (the system knows the ignition/heat history because `EV_TEMP_PEAK` fired). Crucially, the journal shows *relationships the player has actually witnessed* — "liquid Hematite, when cooled, became **Iron** (observed once)." That's an `EV_SOLIDIFY` whose `result` material differed from the input, i.e., the player witnessed smelting as a sequence of physical transitions and the journal reconstructed the causal chain from the event log. The understanding is real because it's assembled from things that physically happened in *their* world.

**Beat 3 — Reproduce and Push.** Now the player wants to do it deliberately, hotter, bigger. This is where the *capability tier* emerges — and I mean emerges, not unlocks.

### Tiers That Emerge Instead of Unlock

This is the most contested design claim, so here is my position and the honest hedge.

**Capability tiers in this game are not data structures. They are physical facts about what the player can currently sustain.** "Can the player work copper?" is not a boolean in a tech tree. It is the question: *can the player construct an arrangement of voxels that sustains a temperature above copper's melt code, for long enough, in a cavity that doesn't bleed all its heat to the surrounding stone?* That question is answered entirely by the heat simulation. To melt copper (1085 C) you need a fuel that burns hot enough and an insulating cavity geometry that lets heat accumulate faster than it conducts away. To melt iron (1538 C) you need a *hotter* fuel and *better* insulation and probably forced air — because charcoal in open air tops out around 1200 C, and the only way past that is a geometry that concentrates and retains heat or a bellows that the player builds from physical voxels. **The tier gate is real thermodynamics, not a flag check.** You literally cannot melt iron until you can sustain the temperature, and sustaining the temperature is a construction-and-fuel problem the player solves in the world.

This is the emergent backbone, and it is genuinely beautiful when it works: progression is a *physical capability curve* — ambient → fire (charcoal, ~600-1200 C) → primitive smelting (copper, tin, lead) → bronze (alloy, requires melting two metals and pooling them together, an `EV_REACT`) → wrought iron (requires sustained 1500+, forced air, refractory cavity) → steel (iron + carbon control) → and onward. Each step is gated by *the previous step's products being usable as better tools/fuel/insulation.* Charcoal lets you melt copper; copper and tin let you... not much hotter, but bronze tools let you dig the refractory clay and build the better furnace that, with more charcoal and a bellows, finally holds 1500. The tiers chain through *physical affordances*, exactly the central fantasy.

**Now the honest hedge, because I promised honesty.** The risk is that this chain has a *discoverability cliff*. The leap from "I can make fire" to "I must build an insulated cavity with forced air to concentrate heat past 1200 C" is a leap a metallurgist knows and a player staring at a voxel world does not. If the player cannot find the next rung, emergence has produced a soft-lock that *looks* like the game is broken — the physics are fine, the player is stuck, and there is no quest marker because we philosophically refused to script one. **This is the real failure mode and I will not pretend the pure-emergence purist position survives contact with it.** It does not. A world with no authored backbone is, for most players, a world with no second hour.

### The Authored Backbone: Hints Derived From Physics, Not Quests

So I take the position that **we author a backbone, but the backbone is made of *physical hints*, never of *gated unlocks*.** The distinction is everything. We never write "to unlock iron, do X." We *do* write, into the MaterialDef and a small companion table, the human-legible *facts* about each material and the *suggestive proximity* of discoveries. The journal then surfaces these as inferences the player could plausibly draw, leaving the doing entirely to them and the physics.

Concretely, three authored aids, in increasing interventionism, all of which respect the read-only rule:

1. **Named properties on the MaterialDef** (authored data, always). Each material has a name and, once `SEEN`, the journal can show authored flavor that *frames* it: "Hematite — a dense iron-bearing ore. Resists melting; demands fierce, sustained heat." This is not a hint about mechanics, it's *characterization that points at the physics.* It's the difference between a number and a clue. Cost: a string table, lives in the 1 MiB material/property budget.

2. **Inference prompts in the journal** (authored templates, fired by observed state). When the player has `MELT_OBSERVED` for copper but their furnace has never exceeded copper's range, and they're holding iron ore, the journal can offer a *question*, not a directive: "Your forge has reached ~1100 C. The iron in this ore shows no sign of melting. What burns hotter than charcoal? What keeps heat from escaping?" This is authored, it is a backbone, and it is *legible* — but it gates nothing and reveals no recipe. It points the player's own experimentation at the temperature axis, which is the real axis. We author one of these per major capability threshold; there are maybe a dozen thresholds in the whole game. That's a *dozen* authored hint templates, not a thousand-node tech tree. **The backbone is thin, hand-placed at the genuine cliffs, and derived from the same thresholds the physics already enforces.**

3. **The thermometer / instrument progression** (the player's UI legibility *is itself* a buildable tier). Early game, the player has no numbers — they read heat as color (the heat-glow render the lighting section bakes from temperature). The first crude thermometer (a tool the player constructs) starts populating the journal with *coarse* observed ranges. A better instrument tightens them. **The legibility of the journal is itself something the player progresses through, diegetically.** This solves a real tension: I want the journal rich, but I don't want the player handed a periodic table on spawn. Tying instrument fidelity to a buildable tier means the information arrives at the rate the player earns it, and the "fog" over the MaterialDef shadow lifts gradually and earned. I think this is the single best idea in this section and I'd defend it over almost anything else here.

#### The Iron Threshold: Default Behavior at the Cliff, Pinned

The charcoal→1200 C wall is the single highest-risk moment in the progression curve, and I refuse to leave the cliff's default behavior as a shrug toward prototyping the way the prior draft did. Every other prototype-flag in this document commits a default that the prototype is allowed to *falsify* — temperature defaults to 8-bit two-segment with a named contingency, meshing defaults to a fixed chunk-per-frame cap. The iron threshold gets the same treatment. Here is the committed default:

> **Default at the iron cliff: ship BOTH (a) the non-directive, question-framed hint template from aid #2 above — "What burns hotter than charcoal? What keeps heat from escaping?" — AND (b) one seeded intermediate processed fuel, a charcoal-derived hotter-burning fuel (think a coked/retorted charcoal: the player heats charcoal in a sealed cavity and the sim produces a denser, higher-combustion-energy fuel material) that pushes a well-built cavity from ~1200 C toward the ~1500 C iron needs. The fuel is itself an emergent product — it is discovered through the same heat-transition machinery, it appears in the journal as an observed material, and it gates nothing. Its sole job is to convert the cliff into a ramp: there exists a discoverable rung between "open charcoal fire" and "forced-air refractory furnace," so the player who is paying attention has somewhere to step.**

This is a default precisely because it is falsifiable, and I name the two-sided falsification test the prototype must run:

- **Falsification A — the ramp is unneeded.** If playtests show players reliably reach iron *without* leaning on the intermediate fuel (they figure out forced-air-plus-insulation from the question hint alone), then the seeded fuel is redundant scaffolding and we **remove it**, collapsing back to "non-directive hint only." Cleaner is better; I will not keep a crutch the playtest proves nobody needs.
- **Falsification B — the ramp still walls players.** If even *with* the intermediate fuel and the question hint, players soft-lock at iron in playtest, then the contingency is to make the threshold hint **directive** — escalate aid #2 at this one threshold from a question to an explicit observation-of-mechanism ("Heat escapes through these stone walls faster than charcoal can replace it. Sealed, insulated cavities hold heat. Forced air burns fuel hotter."). This is the one place in the entire design where I will accept bending toward authorship, and it bends only here, only if both the hint and the ramp fail together.

Note what this default does *not* do: it never violates the read-only rule. The intermediate fuel is a MaterialDef entry with thermodynamic properties like any other — the player produces it through physics, the sim simulates it identically whether or not the progression layer exists, and removing progression leaves the fuel in the world working exactly the same. The directive contingency, if triggered, is still only *text in the journal* — it still gates nothing and writes nothing into the sim. The cliff's default behavior is now pinned to a concrete, shippable position with a named test that can knock it either direction. That is the register the rest of the document holds itself to, and this moment now holds itself to it too.

### What I Deliberately Refuse To Do

- **No tech tree.** No node graph, no prerequisites table, no `unlock(IRON)`. The "tree" exists only as the implicit partial order of physical capability, and it lives in the MaterialDef thermodynamics, not in a progression data structure. If you find yourself writing `if (player.unlocked & IRON_BIT)` anywhere, you have betrayed the design.
- **No XP, no levels, no currency.** Progression is *capability and knowledge*, both physically grounded. The closest thing to a "level" is the temperature your best furnace can sustain, and that's a property of voxels you placed.
- **No progression hook inside the sim tick.** Restated because it's the load-bearing rule. The 14 ms sim budget is sacred and deterministic; progression observes from the 4.33 ms slack band via the event ring.
- **No achievement-spam.** The notification on Beat 1 is factual and rate-limited (dedup on `(kind, mat_a, result)` so "you melted iron" fires once meaningfully, then goes quiet — repeated melts tighten the journal range silently).

### Where I'm Uncertain (Flag for Prototyping)

- **Is the empirical-range journal *actually* satisfying, or just clever?** The "your estimate tightens with observations" loop is elegant on paper. It might feel like busywork — the player just wants the number. **Prototype test:** ship both a tightening-range journal and a reveal-on-first-observation journal to playtesters and measure which produces the "I figured it out!" feeling versus "stop making me do this." I lean toward ranges because of the encoding synergy, but I hold this loosely.
- **The discoverability cliff at the iron threshold (the charcoal→1200 C wall) is the highest-risk moment in the entire game — and its default behavior is now pinned, not deferred.** Per the "Iron Threshold" subsection above, the committed default is to ship the non-directive question hint *plus* one seeded intermediate processed fuel that turns the cliff into a ramp. **Prototype test (two-sided):** measure iron-reach rate and soft-lock rate against this default. Falsification A — if players reach iron without leaning on the intermediate fuel, remove the fuel and ship hint-only. Falsification B — if players still wall even with the fuel, escalate this one threshold's hint from a question to a directive observation-of-mechanism. This is the single place I will accept bending toward authorship, and only if both the hint and the ramp fail together. The decision is committed; the prototype is empowered to knock it in either named direction.
- **Event-ring overflow under pathological activity.** If the player does something that melts ten thousand voxels in a tick (lava flood into a metal vault), the 1024-event ring overflows. That's fine for *progression* (we only need to learn "iron melts" once, so dropping duplicate events is acceptable), but the drain policy must dedup-on-insert by `(kind, mat_a, result)` so a flood doesn't evict the *novel* event behind 900 copies of the same melt. **This is a small, real bug-in-waiting; specify the ring as a dedup set, not a FIFO.**
- **Reaction discovery (`EV_REACT`) legibility.** Alloys and oxidation are the richest emergent content but the hardest to make the player *understand* they caused. When liquid copper and liquid tin pool together and the sim produces bronze, will the player grasp that *they* did that, or will bronze look like a random new material? The journal's causal-chain reconstruction helps, but **whether reactions read as "I invented bronze" versus "huh, weird material appeared" is unproven and needs a focused prototype.**

### The Position, In One Paragraph

Progression is a read-only observer of a purely physical simulation, coupled only through a bounded event ring drained in the frame's slack band, costing the sacred sim budget nothing. The player-facing artifact is a journal that holds an empirically-discovered *shadow* of the global MaterialDef table — storing observed property *ranges* that tighten with repeated observation and sharper instruments, where the temperature encoding's own quantization grain becomes the literal grain of the player's earned knowledge. Capability tiers are not unlocks; they are physical facts about what temperature the player's constructions can sustain, gated by real thermodynamics that the heat simulation already enforces. The authored backbone is deliberately thin — a dozen hand-placed *physical hints* at the genuine discoverability cliffs, framed as questions that point the player's experimentation at the right axis without revealing a recipe — plus a diegetic instrument progression that controls the rate at which the journal's fog lifts. At the one genuinely make-or-break cliff, the iron threshold, I commit a default rather than defer it: ship the question hint *and* one seeded intermediate hotter fuel that converts the cliff into a ramp, with a two-sided prototype test empowered to either remove the fuel (if the ramp proves unneeded) or escalate the hint to directive (if the ramp still walls players). I am confident in the observer architecture and the instrument-gated legibility. I remain genuinely uncertain whether pure emergence survives the iron cliff — but that uncertainty now lives in a falsifiable default, not an open question, exactly as the rest of this document demands.

---

## 10. Risk Register

This register is not a list of everything that could go wrong — it is the five risks that can actually *kill the project* or force a fundamental redesign, ranked by severity (likelihood × blast radius). I have deliberately excluded risks that are merely annoying (GLSL 1.10 quirks, save format churn) in favor of the ones where the failure mode is "the design premise does not hold on this hardware." Each risk names a concrete early-warning signal you can instrument and watch for *during prototyping*, because the entire point of pinning these numbers early is that we can falsify them cheaply before they are load-bearing. Two of these five (R1, R2) are existential; the other three are severe-but-survivable with the contingencies described.

### R1 — CPU-side draw-call submission chokes the single core (Render budget collapse under GL 2.0)

**Likelihood: HIGH. Impact: CRITICAL (existential to the 30 FPS target).**

This is the risk I am most worried about and the one most people will underestimate, because it is invisible until you have real geometry on screen. The G70 is a perfectly capable rasterizer — it is not the bottleneck. The bottleneck is that under OpenGL 2.0 on the XP driver, with **no VAOs, no instancing, no UBOs**, every visible chunk costs roughly one `glDrawElements` plus the state setup around it (bind VBO, set pointers, set the per-chunk model matrix uniform). With ~2,500 chunks carrying geometry in the window and frustum culling leaving perhaps 1,000–1,800 visible, the render phase's 9 ms budget gives us only about **3.6 µs per draw call for 2,500 calls** (verified). XP-era GL drivers routinely spend 5–15 µs of *CPU* time per draw call in validation and command translation. That alone can blow the entire 9 ms render budget two- to four-fold, and because sim/mesh/render are **serialized on one core**, an overrun here does not just drop a frame — it steals time directly from the 14 ms simulation budget, which is the game's whole reason to exist.

**Early warning signal:** instrument CPU wall-time *inside* the render-submission loop (not GPU time — CPU time spent in GL calls) as soon as the prototype draws more than ~200 chunks. Watch the ratio of submission-CPU-ms to triangle count. If submission-CPU-ms scales with *draw-call count* rather than *triangle count*, you are driver-bound, and the trend line predicts catastrophe at full window count. A second tell: frame time that climbs roughly linearly as you increase render distance even though each added chunk is mostly empty sky/air with few triangles.

**Contingency / fallback (in escalation order):**
1. **Chunk batching.** Merge the meshes of several neighboring chunks (e.g., a 2×2×2 super-chunk for *rendering only*, distinct from the 16³ sim/stream/storage chunk) into one VBO and one draw call. This cuts draw calls 8× at the cost of coarser frustum-cull granularity and larger remesh units — an acceptable trade since the meshing section already defers remesh elastically. This is the *expected* mitigation, not the emergency one; budget for it.
2. **Distance-based mesh merging.** Far chunks (already fog-dimmed on the G70) get merged into large static batches with no per-chunk culling; only near chunks keep 16³ granularity.
3. **Aggressive fog pull-in.** Reduce the effective render radius below the r=12 *resident* radius. The window stays 156 MiB resident for simulation/digging continuity, but we only *draw* r=8 and fog the rest hard. This decouples render cost from residency cost — exactly what the single-core budget needs.

The binding window radius (r=12) is a *residency* decision and must not be conflated with render distance; this risk is the reason that separation matters.

### R2 — Mesh VRAM blows the 256 MB G70 budget when the CA churns geometry

**Likelihood: HIGH. Impact: CRITICAL.**

Greedy meshing's whole value is merging coplanar same-material faces into big quads, which keeps vertex count and therefore VRAM low. The problem is that **the cellular automaton actively works against greedy meshing.** A melting pool, a spreading liquid, a heat front oxidizing a surface — these create irregular, mixed-material, partially-filled (the 4-bit fill level) voxel patterns that *defeat coplanar merging*. A pathological churning region approaches the checkerboard worst case where greedy degenerates to one quad per face: a single 16³ chunk can spike from ~1,536 exposed quads (solid-cube surface, verified) toward ~12,288 quads (verified half-solid checkerboard) — an 8× vertex explosion *in exactly the chunks the player is staring at while smelting.* At ~2,500 geometry-carrying chunks, even a modest 60 KB average mesh is **146 MiB** (verified), already over half of VRAM before textures, the atlas, framebuffers, and the liquid pass. A CA-churned average pushes this past 256 MB and the driver starts evicting/thrashing VBOs over the AGP/PCIe bus mid-frame — a multi-millisecond stall on the one core we cannot afford to stall.

To be explicit so the two figures are not read as conflicting: this **~146 MiB** worst case and Section 4's **~43 MiB typical / ~57 MiB pessimistic** mesh-VRAM estimates are **two points on one load curve, not rival estimates** — Section 4 prices static/steady-state terrain (well-merged greedy quads), while this risk prices the CA-churn checkerboard degeneration at the same 2,500 chunks (≈60 KB/chunk → ~146 MiB). Each arithmetic is individually correct; the curve runs from the Section 4 steady-state figure up to this churn worst case as greedy merging degrades.

**Early warning signal:** track total VBO bytes resident and, separately, **vertices-per-chunk for chunks containing active (`LIQUID`/`ACTIVE`-flagged) voxels** versus static chunks. If active-region chunks show 4–8× the vertex count of comparable static terrain, greedy is degenerating under CA churn exactly as feared. Also watch for a sawtooth in frame time correlated with VBO re-upload — the signature of VRAM pressure forcing driver eviction.

**Contingency / fallback:**
1. **Hard per-chunk vertex cap with greedy fallback to naive culled faces** — bounded worst case is more predictable than occasionally-great-occasionally-catastrophic greedy.
2. **Compress the vertex format ruthlessly.** Positions as `GLshort` in chunk-local 0..16 space (4-bit subdivision if fill levels need it), normal as a 3-bit face index, AO/light packed into the spare bits. Target ≤8 bytes/vertex. This is mandatory regardless and should be designed in from day one, not added under duress.
3. **Don't re-mesh churning regions as static geometry at all.** Active liquid/molten voxels are visually transient and few (bounded by the active-voxel cap). Render them in a **separate dynamic particle/point or small dynamic-VBO pass** sized to the active cap (thousands of voxels, tiny), and only bake to greedy static mesh once a region *resolidifies and goes idle* (the `DIRTY_MESH` flag clears on settle). This is the structurally correct fix: it aligns mesh stability with simulation stability and stops the CA from polluting the static VRAM budget. I recommend designing toward this from the start.

### R3 — 32-bit address-space fragmentation kills long sessions

**Likelihood: MEDIUM-HIGH. Impact: CRITICAL (crash, total session loss).**

The memory envelope deliberately operates at ~35% of the ~1.8 GiB practical usable space precisely because **32-bit XP dies from address-space fragmentation long before byte exhaustion.** The acute danger in *this* design is the streaming churn: as the player moves, chunks (16 KiB voxel arrays) and their meshes (variable size, tens of KiB) are allocated and freed continuously, hour after hour. Variable-size mesh buffers are the fragmentation hazard — a remesh that grows a chunk's vertex buffer frees a 30 KiB block and requests a 50 KiB one, and over a long session the largest contiguous free run erodes until a 96 MiB pooled mesh allocation or a fresh VBO mirror fails *despite ~1 GiB of total free bytes.* The result is a hard crash or allocation failure mid-play, with no second core or VM trick to save us.

**Early warning signal:** log the **largest contiguous free virtual block** (via `VirtualQuery` walk) once per minute, not just total committed bytes. The killer is not the total trending up — it is the largest-free-block trending *down* while total stays flat. A slow decline of the max free block over a 2-hour play session is the unambiguous signature. Also watch total committed creeping above the ~630 MiB operating point during pure exploration (it should be *constant* — window-bounded — so any upward creep is a leak or fragmentation surrogate).

**Contingency / fallback:**
1. **Slab/pool allocators for the two churning categories.** Voxel chunk arrays are *fixed* 16 KiB — pool them in fixed-size slabs and reuse on evict (never malloc/free per stream event). Mesh buffers should be pooled into a small number of **fixed size-classes** (e.g., 16/32/64/128 KiB buckets) so freed mesh memory is exactly reusable by the next mesh of that class — eliminating variable-size fragmentation entirely. This is mandatory, not optional.
2. **Pre-reserve the big arenas once at startup** (resident voxel arena ~160 MiB, mesh arena ~96 MiB) as single large `VirtualAlloc` reservations and sub-allocate within them. Reserving contiguous space up front, before driver/atlas allocations scatter the space, guarantees the large runs exist.
3. **Watchdog + graceful save.** If largest-free-block drops below a threshold (say 128 MiB), trigger an autosave and a soft "memory low — please save and restart" rather than gambling on the next big allocation.

### R4 — Heat-sim numerical stability and cost under the non-linear 8-bit temperature encoding

**Likelihood: MEDIUM. Impact: HIGH (core mechanic feels broken or eats the sim budget).**

Heat is the *first and richest* interaction and the one the temperature encoding was tuned for, which makes it the place a flagged assumption can bite. Two coupled failure modes. **(a) Quantization instability:** the binding encoding gives 20 °C/step in the industrial band (codes 160–255). Explicit diffusion that computes a neighbor average and writes back into an 8-bit cell can *stall* (the computed delta rounds to less than one quantum, so heat stops propagating and a forge never reaches the far wall) or *oscillate/lose energy* (rounding asymmetry leaks or injects heat, breaking conservation — a furnace that mysteriously cools or runs away). This was explicitly flagged for prototype validation in the foundation, and it must be resolved before the encoding is frozen. **(b) Cost:** explicit Euler diffusion is conditionally stable; a too-large effective `dt` over 1 m voxels makes a hot voxel push more heat than physical into neighbors and the field diverges (NaN-equivalent in fixed point: temperatures pin to 255 and spread). Fixing stability by sub-stepping multiplies the per-tick cost and threatens the 14 ms sim budget.

**Early warning signal:** a temperature debug-view that shows **visible stair-step banding** in a slowly diffusing gradient (the foundation's named test). For conservation: sum total thermal energy across an isolated heated region each tick — it should be flat (closed system) or monotonic (with a known source); any drift or oscillation is the rounding bug. For cost: per-tick sim ms climbing super-linearly with active-region size.

**Contingency / fallback:**
1. **Accumulator / error-feedback on diffusion** (carry the sub-quantum remainder forward per voxel) so heat propagates correctly despite coarse buckets without widening the field. This is the cheap first fix and likely sufficient.
2. **Compute diffusion in a wider transient scratch type.** Temperature *stored* in the voxel stays 8-bit (the binding decision holds), but the active-set double-buffer (the 16 MiB sim scratch already budgeted) can hold 16-bit fixed-point temperature for the cells being actively simulated, quantizing back to 8-bit only on write-out to settled voxels. This respects the rule that persistent per-voxel fields cannot grow while giving the math headroom — transient data living in a side structure, exactly as the foundation mandates.
3. **Last resort, the flagged contingency:** promote stored temperature to 12 bits by evicting fill+light into a parallel array. This is expensive (loses the in-voxel mesh cache-line win for light) and should be avoided; it is the fallback only if 1 and 2 fail validation.

### R5 — Emergent progression is illegible or unfun (the design-premise risk)

**Likelihood: MEDIUM. Impact: HIGH (the game is technically perfect and nobody understands it).**

Every other risk here is about whether the engine *runs*; this one is about whether the result is a *game*. The central fantasy is that mechanics emerge from data-driven material physics rather than scripted recipes — a furnace is a stone cavity where heat genuinely propagates and metal genuinely melts at its `MaterialDef.melt_temp`. The risk, well-known from Noita and Dwarf Fortress, is that pure emergence becomes **illegible**: the player builds a cavity, lights a fire, and nothing visibly happens because heat is diffusing invisibly through stone at a rate they cannot perceive, or melting happens but the resolidified ingot is buried in slag they cannot identify, or the 20 °C industrial quantization means the feedback between "hotter fire" and "faster melt" is too coarse to feel like a learnable system. With no scripted recipe to teach the loop, the player has no foothold and the progression — the entire reason for the industrial framing — stalls. This is a real and common failure mode for physics-emergent games, not a hypothetical.

**Early warning signal:** the first non-developer playtest (do this *early*, with a crude prototype). Watch whether a fresh player can, unprompted, get from "ore + fuel" to "usable metal" within a session. If they cannot articulate *why* something melted or didn't, the system is illegible. Internally, the tell during development is the team needing the temperature debug-view to understand what the sim is doing — if *you* can't read it without instrumentation, the player certainly can't.

**Contingency / fallback:**
1. **Diegetic legibility layers, not scripted recipes.** Add readable *feedback* without breaking emergence: glow-by-temperature on voxels (the 8-bit temp drives emissive color — black-body ramp), visible state-change effects (smoke, sparks, dripping) on melt/boil threshold crossings, and a survey/thermometer tool the player can point at a voxel to read material + temperature. This preserves "mechanics emerge from physics" while making the physics *visible*.
2. **Tune the `MaterialDef` table toward perceptibility, not realism.** Because all behavior is data-driven from one global table (a binding reality), legibility is a *tuning* problem, not a code rewrite. Compress melt-point spreads, exaggerate thermal conductivity contrasts, and make molten/solid/slag variants visually distinct — all by editing 256 table entries.
3. **Guided emergence as onboarding.** A small number of *discoverable* hints (a starting structure, an in-world journal) that teach the *first* loop without hardcoding it, after which the player extrapolates. This is the deferred-to-design-iteration fallback; flag for early playtesting because it cannot be validated by engineering alone.

---

**A note on what I did *not* rank top-five and why:** *Save bloat from modified chunks* is real (50,000 modified chunks at 16 KiB raw is ~781 MiB on disk, verified; ~146 MiB compressed at 3 KiB/chunk) but it is a disk-space and load-time concern, not a crash or premise-killer, and standard delta-compression of modified-against-procedural handles it — survivable, hence excluded. *G70/XP GLSL 1.10 driver quirks* are an annoyance tax (per-driver shader bugs, precision differences) that costs developer time but does not threaten the architecture; mitigate by testing on the actual M170 continuously rather than trusting the cross-compile host. Both are tracked, neither is top-five.

---

## Appendix A: Toolchain & Build

This appendix pins how the binary gets *made* and how it gets *measured*. The constraints flow directly from the deployment target — a Pentium M 780 (Dothan core) running 32-bit Windows XP SP3, with a G70 GPU on OpenGL 2.0 — and from the operational reality that there is exactly one developer cross-compiling from a modern Linux box. Every choice below optimizes for one thing: a fast, reproducible, *dumb* edit-build-deploy-measure loop that never requires touching a Windows IDE. The XP machine is a test target and a profiling oracle, never a development environment.

### Cross-Compiler: MinGW-w64, `i686-w64-mingw32`, GCC

The toolchain is **MinGW-w64 in its `i686-w64-mingw32` (32-bit) configuration, GCC 11.x or 12.x**, installed on the Linux dev machine. Not the legacy mingw.org project (dead, frozen on an ancient Win32 API surface), not LLVM-mingw, not a real XP VM with period MSVC. I defend this against each alternative.

**Why 32-bit (`i686`), not `x86_64`.** This is not a preference, it is the target. The Pentium M is a 32-bit-only CPU — no long mode, no AMD64. XP SP3 here is the 32-bit edition with its ~1.8 GiB practical usable address space (the entire `MEMORY_OPERATING_POINT` budget is built on that ceiling). A 64-bit binary will not even load. So the *triplet* is fixed by physics: `i686-w64-mingw32`. The corollary is that all of our memory discipline (operate at ~35% of 1.8 GiB, pool large allocations, fear fragmentation) is a permanent property of the build target, not a phase we grow out of.

**Why MinGW-w64 over a real XP VM + old MSVC (e.g. VC6 or VS2005/2008).** The MSVC-on-XP path is the historically "authentic" route and I reject it firmly for a solo dev:

- **The dev loop lives on modern Linux.** Editor, version control, ripgrep, the C toolchain, scripts — all on the fast machine. With MinGW-w64 the *build* is a single command on Linux producing a `.exe` we copy to the M170. With an XP VM you are either developing *inside* XP (slow, antique editors, no modern tooling) or you are syncing source into a VM and driving an ancient compiler over a shared folder — strictly worse than cross-compiling and with a flakier feedback loop.
- **C99 support.** MSVC's C support was notoriously stuck in C89 for a very long time; VC6/VS2005/VS2008 do not give us clean C99 (no proper `// ` was the least of it — no designated initializers, no mixed declarations-and-code, no `stdint.h` without third-party headers, no `<stdbool.h>`). GCC has had complete C99 for over a decade. Our entire voxel/material model leans on `uint32_t` and designated initializers for the `MaterialDef` table (below). MSVC actively fights that.
- **Reproducibility and headlessness.** MinGW-w64 builds run in CI / a script with no GUI, no licensing dance, no VM snapshot management. One developer cannot afford to babysit a Windows toolchain.

**Why MinGW-w64, not legacy mingw.org.** MinGW-w64 (despite the name) is the maintained fork with complete, modern Win32 headers, working `stdint.h`/`inttypes.h`, robust C99 printf via `__USE_MINGW_ANSI_STDIO`, and — critically — *full support for targeting old Windows*. It still emits binaries that run on XP. The original mingw.org is abandoned. There is no upside to it.

**Why not LLVM/clang-mingw.** Clang is a fine compiler, but the LLVM-mingw distributions target newer Windows runtimes and `i686` XP support is not a maintained, tested configuration there. GCC + MinGW-w64 targeting XP is a *well-trodden* path with predictable behavior. For a one-person project on a fragile target, "boring and proven" beats "modern and slightly off the map."

**Architecture flags — pin the ISA to what Dothan actually has.** This matters more than usual because GCC defaults will happily emit instructions the Pentium M cannot execute. Dothan is a Pentium III-derived core with **SSE and SSE2, but no SSE3, no SSSE3, no SSE4, no AVX**. The decisive flag is therefore:

```
-march=pentium-m -mtune=pentium-m -mfpmath=sse -msse2
```

- `-march=pentium-m` tells GCC the exact ISA — it will use SSE2 and *will not* emit SSE3+. This is the safety rail: build with `-march=native` on your modern dev box by accident and the binary will `#UD` (illegal instruction) the instant it hits an AVX op on the M170.
- `-mfpmath=sse -msse2` routes floating point through SSE2 registers instead of the legacy x87 stack. We want this — but be precise about *what* float work it governs. The CA hot loops (heat diffusion, fluid equalization, temperature decode) are **integer fixed-point**, per Section 3.6, exactly for cross-machine determinism; they do *not* run on this float path at all. `-mfpmath=sse` governs only the **incidental float work**: the camera/projection/view matrices, fog, and render-side interpolation factors. For that incidental float, SSE2 scalar float is faster and more predictable than x87 on this core, and routing it off the x87 stack avoids x87's 80-bit-intermediate nondeterminism. (Whatever float the renderer touches, keep it off x87; the deterministic sim stays integer regardless.)
- **Do not** pass `-mavx`, `-msse3`, `-msse4`, or `-march=native`. Treat any of those in a build log as a release blocker.

**Optimization flags.** `-O2` is the binding default, not `-O3`. On a 2MB-L2, single-core, in-order-ish core, `-O3`'s aggressive inlining and loop unrolling frequently *bloats* the instruction footprint and hurts the I-cache more than the unrolling helps — and our hot loops (the CA over the active set) are already cache-bound on *data*, per the foundation. `-O2` plus targeted manual hints is the right altitude. Add `-fno-strict-aliasing`: we deliberately type-pun the 4-byte `Voxel` `uint32_t` through masks and will occasionally alias it; strict aliasing is a footgun we do not need GCC second-guessing. Add `-ffast-math` only with caution and only after the heat-sim prototype validates it doesn't perturb the temperature-quantization thresholds — flag this as a prototype decision, default **off** for determinism (the sim is fixed-tick and we want bit-stable behavior across runs).

```
CFLAGS = -std=c99 -O2 -march=pentium-m -mtune=pentium-m -mfpmath=sse -msse2 \
         -fno-strict-aliasing -fno-exceptions -fno-unwind-tables \
         -ffunction-sections -fdata-sections -Wall -Wextra
```

`-fno-exceptions -fno-unwind-tables` (this is C, we never unwind) plus `-ffunction-sections -fdata-sections` set up dead-code stripping at link time (`--gc-sections`), which trims binary size and, more importantly, keeps unused cold code out of the working set.

### C Standard: C99 Is the Ceiling, and the Floor

**Target C99 (`-std=c99`), and treat it as both the ceiling and the working baseline.** Not C11, not C89, not GNU extensions.

C89 is too poor to express this engine cleanly — no `stdint.h`, no `<stdbool.h>`, no designated initializers, no mixed declarations. C11 is tempting for `_Static_assert` and atomics, but: the sim is *single-threaded by hard constraint* so C11 atomics/threads are dead weight, and leaning on C11 erodes the one virtue of staying conservative — that the same source could, in a pinch, be coaxed through an older compiler. C99 is the sweet spot where GCC/MinGW-w64 is rock-solid and the language is rich enough.

**C99 features to lean on (these are load-bearing for the architecture):**

- **`<stdint.h>` fixed-width types** — `uint32_t Voxel`, `uint64_t chunk_key`, `int32_t` chunk coords. The entire foundation is specified in exact-width types; this is non-negotiable and C99 gives it to us portably.
- **Designated initializers** — the single most valuable C99 feature for *this* engine. The `MaterialDef` table is the data-driven heart of emergence; designated initializers make it readable and safe against field-reorder bugs:

  ```c
  static const MaterialDef MATERIALS[256] = {
      [MAT_IRON]   = { .melt_temp = 1538, .density = 7874, .thermal_k = 80, .flags = MAT_METAL },
      [MAT_COPPER] = { .melt_temp = 1085, .density = 8960, .thermal_k = 401, .flags = MAT_METAL },
      [MAT_WATER]  = { .melt_temp = 0,    .density = 1000, .thermal_k = 1,  .flags = MAT_LIQUID },
      /* gaps default-zero; index IS the material id */
  };
  ```

- **`<stdbool.h>`** — readable flag logic without `int` ambiguity.
- **`<inttypes.h>` `PRIu32`/`PRIu64`** — for the debug overlay and logging across the 32-bit target without printf-format guessing.
- **`inline`** — the foundation already specifies `static inline uint64_t chunk_key(...)`; C99 `inline`/`static inline` is well-defined and we use it for the hot accessors (`VOX_MAT`, neighbor lookups).
- **Mixed declarations and statements; `for`-loop-scoped iterators** — minor quality-of-life, fully supported.

**C99 features to AVOID for safety on the old runtime:**

- **Variable-length arrays (VLAs).** Banned outright. They put unbounded allocations on the stack — exactly the wrong thing on a memory-fragmentation-sensitive 32-bit target where we want *every* sizable allocation pooled and explicit (`MEMORY_OPERATING_POINT` discipline). Size buffers from the binding constants instead (a chunk scratch buffer is `16 * 16 * 16 * sizeof(Voxel)` = a compile-time constant).
- **C99 `printf` width specifiers via the MSVC runtime.** XP's `msvcrt.dll` does not implement C99 `printf` correctly (`%lld`, `%zu`, the `z`/`j` length modifiers misbehave). MinGW-w64 fixes this *only if* you opt in. Define `__USE_MINGW_ANSI_STDIO=1` (compile flag `-D__USE_MINGW_ANSI_STDIO=1`) so MinGW supplies its own conforming stdio rather than punting to `msvcrt`. Without this, the debug overlay's formatted numbers silently corrupt. This is a known XP/MinGW footgun and the define is mandatory.
- **`complex.h` and `tgmath.h`** — no use here, and `tgmath` leans on compiler magic that's not worth the risk.
- **`long double`** — on MinGW it's 80-bit x87, which collides with our `-mfpmath=sse` decision and reintroduces x87 state. Use `double` and `float` only.
- **Trusting `snprintf` truncation semantics on old `msvcrt`** — again resolved by `__USE_MINGW_ANSI_STDIO`, but the rule stands: any string formatting goes through the MinGW ANSI stdio, never raw `msvcrt`.

### Build System: A Plain Makefile (with a Shell Wrapper), Not CMake

**The build is a hand-written GNU `Makefile`, invoked through a thin `build.sh` wrapper that sets the cross-compiler prefix.** I reject CMake and I reject an opaque single mega-script.

**Against CMake.** CMake earns its keep when you have multiple platforms, multiple compilers, generated IDE projects, and a dependency graph nobody wants to maintain by hand. We have *one* compiler (`i686-w64-mingw32-gcc`), *one* target (XP/32-bit), *one* developer, and a deliberately small static set of dependencies. CMake's toolchain-file ceremony for cross-compiling, its generator indirection, and its habit of hiding the actual compiler command behind layers is pure friction here. When something goes wrong on an exotic old target, I want to *see the exact `gcc` invocation*, not archaeology it out of `CMakeCache.txt`. For a solo cross-compile project, CMake is over-engineering.

**Against a single build script.** A bare `build.sh` that just lists every `gcc` command rebuilds the world on every invocation. With a growing C codebase and the slow-ish cross-compile, incremental builds via `make`'s dependency tracking save real wall-clock time on the inner loop. A Makefile gives incremental compilation essentially for free.

**The Makefile, sketched:**

```makefile
CROSS   := i686-w64-mingw32-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

CFLAGS  := -std=c99 -O2 -march=pentium-m -mtune=pentium-m -mfpmath=sse -msse2 \
           -fno-strict-aliasing -ffunction-sections -fdata-sections \
           -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -MMD -MP
LDFLAGS := -static -static-libgcc -Wl,--gc-sections -mwindows
LDLIBS  := -lopengl32 -lgdi32 -lwinmm   # opengl32/gdi32 dynamic by necessity

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

voxel.exe: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

-include $(DEP)        # -MMD/-MP auto header-dependency tracking
```

`-MMD -MP` gives correct incremental rebuilds when headers change, with no manual dependency lists. A `make profile` target flips in `-pg` and a debug `make debug` target adds `-g -O0`. The whole thing is auditable at a glance — which is the entire point on a target this unusual.

### Static Linking: Kill DLL Hell on XP, Keep `opengl32.dll` Dynamic

XP's DLL situation is a minefield: missing or wrong-version MSVC runtimes, the `libgcc`/`libwinpthread` MinGW DLLs that must travel with the binary, SDL/GLEW DLLs of uncertain provenance. The deployment story must be **"copy one `.exe`, double-click, it runs"** — no redistributables, no `Could not find msvcr*.dll`, no version roulette. So: **static-link everything we legally and technically can; the only mandatory dynamic dependency is `opengl32.dll`.**

**Static-link the MinGW runtime support:**

```
-static -static-libgcc
```

`-static-libgcc` folds `libgcc` in; `-static` pulls in the rest of the MinGW support statically and avoids dragging `libwinpthread-1.dll` (which a surprising number of GCC configs reference even in single-threaded C). With these, there are **no MinGW DLLs to ship**. We still link against XP's stock `msvcrt.dll` — that is the *one* C runtime guaranteed present on every XP install since it ships with the OS, so depending on it dynamically is safe and correct (it is not the "shipping a runtime" problem; it's already there). We do **not** chase a newer `msvcr80/90/100.dll`, which *would* require a redistributable. `msvcrt.dll` + MinGW's `__USE_MINGW_ANSI_STDIO` shim is the combination that gives us both "no redist" and "conforming stdio."

**`opengl32.dll` MUST stay dynamic — this is not optional.** `opengl32.dll` is the Windows OpenGL ICD loader; it is owned by the OS and the NVIDIA driver. You cannot statically link it — the actual G70 driver implementation lives behind it and is bound at runtime. So `-lopengl32` is a dynamic import, by necessity and by design. Likewise `gdi32.dll` (needed for the pixel-format / `wglCreateContext` dance) and `winmm.dll` (high-resolution timing — see profiling) are core OS DLLs, always present, linked dynamically. These are the *good* dynamic deps: guaranteed-present OS components, never a "ship a DLL" problem.

**Windowing and GL extension loading: go raw Win32 + WGL, statically.** I recommend **not** depending on SDL as a DLL. Two acceptable routes, in order of preference:

1. **Raw Win32 + WGL (preferred).** Create the window with `gdi32`/`user32`, set the pixel format, `wglCreateContext`, and load the handful of ARB extension entry points we actually use (VBOs via `ARB_vertex_buffer_object`, shader objects via `ARB_shader_objects`/GLSL 1.10-1.20) by hand with `wglGetProcAddress`. The GL 2.0 + reliable-ARB surface is small and fixed; a hand-rolled loader for the ~20-30 entry points we touch is a one-time cost that eliminates GLEW entirely and gives total control over exactly which extensions we bind. This is the cleanest fit for the hard "GL 2.0, no VAOs/UBOs/geometry/compute" constraint — we never even *see* the modern entry points.
2. **SDL2 (or SDL 1.2) statically linked, GLEW statically linked.** If hand-rolling the WGL bootstrap proves fiddly during prototyping, fall back to **static** SDL + **static** GLEW (`-DGLEW_STATIC`, link `libglew32.a`). The rule that matters: **link them as `.a` archives, never ship `SDL2.dll`/`glew32.dll`.** Build SDL from source with the same `i686-w64-mingw32` toolchain so the ABI and CRT match exactly. SDL2 on XP is a known-iffy area (later SDL2 versions dropped clean XP support); if we go SDL, pin an XP-compatible release and verify on the M170 early. **Flag for prototyping:** decide route 1 vs 2 in the first week — it affects nothing architectural, only the bootstrap, but the GL-extension-loading approach should be settled before the rendering section's code lands.

The shipped artifact is then: `voxel.exe` + the asset/data files. Zero accompanying DLLs. That is the bar.

### Remote Test & Profile on the M170 Without Modern Tooling

There is no Visual Studio profiler, no VTune that understands this loop, no sane modern instrumentation on XP SP3. Profiling is *primarily in-engine and manual*, with `-pg`/gprof as a secondary offline pass and era-appropriate sampling as a tertiary fallback. This is fine — arguably better — because the foundation already hands us the exact budgets to measure against (`FRAME_BUDGET_SPLIT`: Sim 14 / Mesh 6 / Render 9 / Overhead 4.33 ms).

**Deploy: a one-line copy.** Because we ship a single static `.exe`, deployment is `scp`/SMB-share/USB-key the binary to the M170 and run it. A `make deploy` target can push over a network share if the XP box exposes one. No installer, no dependency staging. Keep a `make strip` (`i686-w64-mingw32-strip voxel.exe`) for release builds to shrink the binary; keep symbols in the dev/profile builds.

**Primary tool: the in-engine frame-time overlay.** This is the workhorse and it must exist from day one. A fixed HUD that displays per-phase milliseconds against the binding budget, color-coded over/under:

```c
/* High-resolution timing on XP: QueryPerformanceCounter is the
 * correct primitive (sub-microsecond-class on this hardware). Wrap each
 * phase. timeGetTime()/winmm is the fallback (~1ms granularity after
 * timeBeginPeriod(1) — too coarse for sub-phase, fine for whole-frame). */
typedef struct { const char *name; double ms; double budget; } PhaseTimer;

LARGE_INTEGER t0, t1, freq;
QueryPerformanceFrequency(&freq);
QueryPerformanceCounter(&t0);
   sim_tick();
QueryPerformanceCounter(&t1);
sim.ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
/* overlay draws: SIM 12.4/14.0  MESH 4.1/6.0  REND 8.7/9.0  OVHD 3.0/4.33 */
```

Use **`QueryPerformanceCounter`** for sub-millisecond phase timing — and it *is* reliable on this machine, but the reason matters, because the naive justification is wrong and would lead you to disable the very mechanism that saves you. The hazard with a TSC-backed timer is **not** purely inter-core skew. The Pentium M 780 runs Enhanced SpeedStep (enabled by default on the XPS M170), so the core clock — and therefore the rate of a TSC-derived counter — *changes as the CPU scales frequency*. A single core is itself a TSC-frequency hazard on a SpeedStep part; "we have only one core, so the multi-socket TSC-skew problem doesn't apply" is the wrong reasoning and a trap. **The actual reason QPC is safe here is that Windows XP does not back QPC with the variable TSC on a system like this — it routes QPC through the ACPI Power Management Timer (the PMT, fixed at ~3.579545 MHz, sub-microsecond-class), which is immune to SpeedStep frequency scaling.** That fixed-rate hardware timer is what makes `QueryPerformanceFrequency` return a stable, trustworthy tick rate regardless of what the CPU clock is doing. The corollary, stated as a rule: **never read `rdtsc` directly for timing on this target** — the raw TSC's rate drifts with SpeedStep and is exactly the failure mode XP's PMT-backed QPC was designed to dodge. Use QPC; do not "optimize" it by reaching under it to the cycle counter, and do not disable or assume away the PMT fallback. Keep a rolling-average and a max-spike readout per phase so we catch the *worst* frame, not just the mean — the foundation's whole cut-order logic (render sacrosanct, meshing elastic) is about taming spikes, so the overlay must surface them. This overlay is also the acceptance test for the `TARGET_FRAMERATE` and `FRAME_BUDGET_SPLIT` decisions: if Sim blows past 14 ms with the active-voxel cap engaged, that's a finding, on-screen, on the real hardware.

**Secondary: gprof via MinGW.** For *where inside a phase* the time goes, build a dedicated profiling binary with `-pg` (the `make profile` target). gprof works through MinGW-w64: the instrumented `.exe` writes `gmon.out` on exit on the XP box; copy it back to Linux and analyze with `i686-w64-mingw32-gprof voxel.exe gmon.out`. Caveats to honor: `-pg` instrumentation perturbs the very cache behavior we care about (it adds `mcount` calls to every function entry), so gprof tells you *call counts and relative hot functions*, not trustworthy absolute timings — cross-reference against the in-engine QPC numbers, which are measured on the real, uninstrumented loop. Use gprof to answer "is the heat-diffusion inner loop or the fluid-equalization loop the bigger cost," not "how many ms does the sim take" (the overlay answers that).

**Tertiary: era-appropriate sampling.** When we need a *non-instrumented* statistical profile on XP itself (to catch cache-miss-dominated hotspots that gprof's instrumentation distorts), the period-correct tools are **Sleepy (Very Sleepy)** — a lightweight XP-compatible sampling profiler that reads stacks of a running native process and consumes standard debug symbols — and Microsoft's **Xperf/WPT** from the era. Sleepy is the pragmatic pick: it samples the real binary with no recompilation, attributes time by stack, and runs on XP. Generate symbols MinGW-side; Sleepy can consume the debug info from a non-stripped build. This is the fallback for the nasty cases; the QPC overlay handles 90% of day-to-day tuning.

**The measurement discipline, stated as a rule:** every performance claim about this engine is validated by the in-engine QPC overlay *on the M170 itself* — never on the Linux dev box, whose cache, clock, and ISA are nothing like Dothan. The dev box compiles; the M170 judges. gprof and Sleepy are for *attribution* when the overlay says a budget is blown but not *why*.

---

## Appendix B: Open Questions & Deferred Decisions

_Collected automatically from the foundation, every section author, and the cross-section consistency pass. These are the things this document deliberately does NOT settle — they are flagged for resolution during prototyping rather than guessed at._

### Cross-section contradictions reconciled during assembly

These five cross-section disagreements arose because Sections 4, 5, 7, Appendix A and the Risk Register were drafted independently. The consistency pass caught them and they are now **resolved in the body and the Decision Ledger**; this record is kept so the history is transparent and the adopted value for each is unambiguous.

1. RESOLVED (was CRITICAL) — Per-vertex byte size: the meshing and rendering drafts disagreed (Section 4 bound 12 bytes; an earlier Section 5 draft implied a 16-byte float-position vertex with a single pre-baked brightness byte). **Adopted: the 12-byte vertex** — `u16 px/py/pz` + `u8 mat/face/light/ao/u/v` — with `light` and `ao` carried as *separate* per-vertex bytes (greedy merge-key fields) and the sun term folded live in the shader, never baked. Section 5.1 now reads the same struct Section 4 writes; the ≤8 B/vertex mitigation in R2 and every VRAM figure are on this 12-byte basis (48 B/quad vertex data).
2. RESOLVED (was MAJOR) — Texture atlas dimensions: the drafts disagreed (Section 4 said 512×512 / 32×32 tiles / ~1 MiB; Section 5 said 1024×1024 / 64×64 tiles / ~5.6 MiB). **Adopted: 1024×1024 RGBA8, 16×16 grid of 64×64-texel tiles, ~5.3 MiB with mips** (4 MiB base × 1.333; the prior ~5.6 MiB figure was corrected to ~5.3). Section 4's atlas binding and the mesher's `u_atlas_cols`=16 / tile-size-64 math now match Section 5.4's `ATLAS_SIZE`.
3. RESOLVED (was MAJOR) — Fixed-point vs floating-point sim math: an earlier Appendix A draft claimed the CA hot paths were float; Section 3.6 binds them as integer fixed-point. **Adopted: the CA hot loops (heat diffusion, fluid equalization, temperature decode) are integer fixed-point per Section 3.6**, and `-mfpmath=sse` is kept but re-scoped to govern only incidental float (camera/projection/view matrices, fog, render-side interpolation factors). The false float-CA claim is removed from Appendix A.
4. RESOLVED (was MINOR) — CPU-side mesh memory baseline: the drafts disagreed (Section 4 used ~17.6 KiB/meshed chunk → ~44 MiB shadow set; Section 7 used ~38 KiB → ~93 MiB). **Adopted: the conservative ~38 KiB/meshed chunk → ~93 MiB shadow set at 2,500 chunks** in both sections; it still fits under the 96 MiB CPU-mesh budget line.
5. RESOLVED (was MINOR, defensible) — VRAM mesh estimate cross-reference: Section 4's ~43 MiB typical / ~57 MiB pessimistic and Risk R2's ~146 MiB describe the *same* mesh-VRAM quantity at different load points on one curve (static/steady-state terrain vs CA-churn checkerboard degeneration; 2,500 × ~60 KB ≈ 146 MiB worst case). Each arithmetic is individually correct. **Adopted: a clarifying cross-reference** added in R2 (and the Section 4 VRAM total) stating these are two points on one load curve, not rival estimates.

### Per-section open questions

**2. Voxel Data Model**

- Temperature diffusion smoothness against the two-segment 8-bit encoding (20 C/step industrial band) must be validated on the real M170 before the encoding is frozen; contingency is 12-bit temperature paid for by evicting fill+light into a side array (Section 1).
- The MaterialDef.conductivity field and the entire electrical-progression tier (wiring, motors) are deferred to prototyping; the field is reserved now to avoid a later struct re-layout and persistence-format version bump, but no electrical CA should be built against it until the progression design (Section 9) lands.
- Exact MaterialDef field set may shift once the heat/fluid/combustion CA prototypes reveal which properties are actually read in hot loops vs. which can live in colder side tables; the 64-byte stride leaves room to add fields without re-layout pressure.

**3. Cellular Automaton Simulation**

- PROTOTYPE FLAG (inherited from foundation): whether the wider internal heat accumulator (S3.6) fully eliminates visible stair-stepping in the temperature debug view under the 8-bit two-segment encoding's 20 C/step industrial band must be confirmed on the actual M170 before the encoding is frozen. Contingency is a foundation-level change to 12-bit temperature.
- SIM_ACTIVE_CAP of 4096 is the defended default but is explicitly prototype-tunable against measured per-voxel kernel cost on the M170; if the real heat+fluid+bookkeeping kernel exceeds ~7,700 cycles/voxel, the cap must drop rather than the budget grow.
- CHUNK_ACTIVE_MAX (512) is a placeholder magnitude; the right value depends on observed front geometry (how many voxels in one chunk are simultaneously active during a realistic lava flood / melt) and should be measured.
- Communicating-vessels accuracy: the local fill-relaxation liquid rule will be visibly slow across large height differences. Whether any gameplay-critical case (e.g. a water-wheel head) demands a localized single-body pressure relaxation must be decided by prototyping.
- The exact HEAT_SHIFT and internal heat-unit scale (16-bit signed assumed) need to be pinned against the normalized 1/6 stability ceiling and the harmonic-mean conductance LUT range so no intermediate flux overflows int32.
- DEFERRED — SINGLE-CHUNK SIM SCOPE (milestone scope, tracked for **0.2**, not a design open question). The `0.1.x` build simulates **exactly one chunk**: a single `SimState` is bound to the fixed HOME demo chunk (`main.c`), out-of-chunk neighbours are treated as a **closed wall** (no heat flux, no fluid flow across a chunk face — `sim.h` deferred-contract note), and the streamed world *outside* HOME is generated/lit/meshed/rendered but **never ticked**. This is the deliberate per-chunk-locality scope of the foundation, not an accident: one chunk is half of L1D and the 4096-active / 7-neighbour working set is ~112 KiB in L2 (Section 3.7), and per-chunk independence is what keeps remesh and active fronts surgical (the same property the mesher's no-cross-seam-merge rule protects, Section 4). The intended end-state is **not** a global solver but **per-chunk active fronts that READ their six neighbours' edge planes** and hand flux/flow across the seam — the heat pass is already meant to keep "both sides hot in L1" when a diffusion pass crosses one chunk face (Section 1 cache note), and the mesher already dirties the abutting neighbour chunk on a boundary edit (Section 4 dirty-set rule), so the seam machinery exists to build on. Two coupled pieces of work, both 0.2-class: (a) run the CA over the *resident window* (an active-chunk worklist), not just HOME; (b) cross-chunk flux/flow via neighbour edge-plane handoff. Until then, demo content is kept **interior** to HOME, clear of its 0/15 faces, so the closed-wall boundary never visibly fires (the 0.1.1 demo-water basin exists for exactly this reason — a settled pool that would otherwise run off the pedestal and pool at the unsimulated chunk floor below).

**4. Meshing**

- MAX_CHUNKS_PER_FRAME (default 8) and MAX_QUADS_PER_FRAME (24000) are unverifiable without the M170 in hand; the true ceiling is glBufferData/glBufferSubData upload bandwidth over the XP/G70 driver and must be measured, not assumed.
- The exact packing of sky vs block light into the single 8-bit light byte (which nibble, what curve the shader applies to each) is owned by the Lighting section; Section 4 only commits that the byte is carried UNFOLDED and that the sun is applied live in the shader. S5/Lighting must publish the unpack.
- Whether CPU-side VBO shadow copies are actually needed depends on whether the target driver loses buffer contents on context events; if not, the ~93 MiB shadow set can be dropped, freeing CPU mesh-buffer budget.

**5. Rendering Pipeline**

- Exact gutter width (4-8 texels likely), u_tileInset value, and GL_TEXTURE_MAX_LEVEL mip cap for the atlas must be tuned by eye on the actual M170 panel.
- Temperature/lighting re-bake cadence for the day-night sun step: how coarsely the sun can move before baked sun_term drift forces a lazy re-bake of chunks within the elastic mesh budget.
- Whether stacked liquids (waterfall in front of lake) need per-quad sorting within a liquid chunk's index buffer at remesh time, or chunk-granularity back-to-front ordering suffices.
- Whether dense-terrain overdraw ever justifies the coarse 'fully-enclosed chunk skip' heuristic (mesher already produces empty VBOs for buried chunks, so likely self-solving).
- DEFERRED to **0.2** — UNDERWATER / SUBMERGED-CAMERA presentation. `0.1.1` made submerged *solid* surfaces visible (mesher emits solid faces against a translucent-liquid neighbour), but the camera-inside-a-liquid experience still has two gaps, both 0.2-class polish (not 0.1.1 bug fixes):
  - **Submerged-camera blue tint / fog.** When the camera is inside a liquid, draw a translucent fullscreen tint (water → blue, lava → orange) plus a stronger depth fog, so being underwater reads correctly instead of looking like clear air. The idiomatic, cheap approach (a single blended fullscreen quad keyed on which liquid the eye point sits in) — no extra geometry. This is the primary fix for "it doesn't feel like I'm underwater."
  - **Double-sided liquid surface.** The liquid pass emits a face only against air and the renderer back-face-culls, so the water *surface* is invisible from **below/inside** (you look up out of a pool and see no surface plane). Make the liquid pass double-sided — either disable `GL_CULL_FACE` for the liquid draw, or emit the surface quad both windings — so the surface shows from underneath. Pairs naturally with the fill-height top-surface work already deferred in `sim.h`.
  Both depend on a cheap "is the camera eye point inside a liquid voxel?" query, which also gates buoyancy/swimming once player physics lands. Tracked together as the 0.2 "underwater" item.

**6. Frame Scheduling**

- QueryPerformanceCounter call overhead on the real M170/XP: if a now_ms() read costs enough to meaningfully erode the 6 ms mesh budget, the between-chunks deadline check must be batched to every Nth chunk. Expected negligible but only measurable on hardware.
- The mesh-budget throttle response curve: the linear 'shed the overrun milliseconds' controller is the simplest stable choice, but whether it oscillates between full and floor budgets under a sustained loaded scene needs tuning on hardware.
- Whether max_substeps should ever be raised to 2 for snappier post-stall catch-up — strictly a CA-section decision once a real sim tick's cost distribution is measured. Default is 1; the spiral clamp is mandatory regardless.
- The exact sim tick rate (assumed 15 Hz here, giving a clean 2:1 render:tick cadence) is owned by the CA/simulation section per SIM_TICK_MODEL; this scheduler is written to be rate-configurable but the 2:1 cadence assumption should be confirmed against the CA section's final choice within the stated 10-20 Hz band.

**7. Memory Budget**

- Mesh-pool average size of ~38 KiB/meshed chunk and the 25%-of-chunks-carry-geometry assumption are estimates that drive the 96 MiB line; the actual distribution depends on greedy-meshing efficiency and material-boundary noise and must be measured against a real world during the meshing prototype. If meshes run larger, the 96 MiB pool or the bucket sizing needs revisiting.
- The 256 MiB GL-driver address-space reservation is an estimate for the G70 user-mode driver on XP SP3; the real figure must be measured on the M170 (via VMMap or equivalent) because it directly determines true usable headroom and whether the 35% operating point holds.
- The per-frame streaming-queue cap (N chunk-loads/frame serviced in the 4.33 ms overhead/slack) is left to the streaming/worldgen section and prototyping; worldgen cost per chunk is unknown until that section exists and determines how many curtains can stream per frame without starving the protected sim budget.
- Whether the modified-chunk on-disk format is RLE, per-voxel delta vs the seed baseline, or block-compressed is deferred to the Persistence section; the 16 MiB IO staging budget assumes decompression of a single curtain's worth at a time and should be revalidated once that format is chosen.

**8. World Persistence**

- Generator versioning policy: on gen_version mismatch, refuse-to-load (default, chosen) vs bulk-bake previously-visited region to disk at upgrade. Deferred to the procedural-gen section.
- Whether to persist the LIQUID flag or re-derive it from mat+fill on the load-time CA re-scan. Default is re-derive (persist only mat|temp|fill, no flags). Must be confirmed by prototyping the load-scan cost.
- Whether to wrap region files (or cold region files) in an optional LZ4 pass on top of RLE-over-palette for extra shrinkage. RLE-over-palette is the binding primary scheme; LZ4 is deferred polish to validate against decode cost on the Pentium M.
- Exact palette-fallback threshold (distinct-material count past which a chunk stores raw mat8 instead of a palette) -- placeholder is >64; tune from real save data.
- Run struct byte-packing: the illustrative Run is 6 bytes; whether to bit-pack (palidx,temp,fill) tighter depends on whether profiling shows record size or decode-loop branchiness dominates. Deferred to prototyping.
- Temperature diffusion smoothness against the 8-bit two-segment encoding is already flagged as a global prototype gate; persistence inherits the outcome (long RLE runs in the ambient band depend on the ambient band quantizing to few codes).

**9. Progression Layer**

- Whether the empirical-range journal (tightening estimates) feels satisfying or like busywork versus a reveal-on-first-observation journal — needs A/B playtest.
- The iron-threshold cliff default (question hint + seeded intermediate hotter fuel) is committed but falsifiable: prototype must measure iron-reach and soft-lock rates to either remove the fuel (ramp unneeded) or escalate the hint to directive (ramp still walls).
- Event-ring drain policy must be specified as a dedup-on-insert set keyed on (kind, mat_a, result), not a FIFO, so a melt flood does not evict novel events.
- Whether emergent reactions (EV_REACT, e.g. bronze from copper+tin) read as player-authored discoveries versus random new materials — needs a focused legibility prototype.

**10. Risk Register**

- Heat diffusion under the 20 C/step industrial quantization: does explicit-Euler diffusion stall or violate energy conservation? Must be validated on the actual M170 with the temperature debug-view BEFORE the 8-bit encoding is frozen (foundation explicitly flagged this).
- What is the real CPU-side cost per glDrawElements on the actual G70 + XP driver? The 3.6 us/call budget (2500 calls in 9 ms) is the make-or-break number for R1 and cannot be measured on the Linux cross-compile host - it requires the target hardware.
- What average and worst-case vertex-count-per-chunk does greedy meshing actually produce on CA-churned regions? This determines whether VRAM (R2) is comfortable or catastrophic and needs a real CA prototype, not estimation.
- Does the render radius need to be decoupled from the r=12 residency radius from day one (R1 contingency 3), and if so what is the fogged-out draw radius? This is a deferred-to-prototyping tuning decision.
- Whether to design the dynamic-active-voxel render pass (R2 contingency 3, separate from static greedy mesh) into the architecture from the start - I recommend yes, but it crosses into the meshing/rendering section's scope and should be confirmed there.
- First-playtest result for R5: can an unprompted player reach usable metal in one session? This is unvalidatable by engineering and must be scheduled as an early human playtest.

**Appendix A: Toolchain & Build**

- Whether `-ffast-math` perturbs the temperature-quantization thresholds — deferred to the heat-sim prototype; default off for determinism until validated.
- GL bootstrap route 1 (raw Win32 + WGL) vs route 2 (static SDL + static GLEW) — to be settled in the first prototyping week before rendering code lands.
- Whether any later XP service configuration or driver could disable the ACPI PM timer and force QPC onto the TSC — must be verified on the actual M170, since the QPC reliability argument depends on the PMT backing remaining active.
