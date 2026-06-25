# Voxel Engine 0.5 — "Finite Flowing Water" — Implementation Plan

> Status: **DRAFT for sign-off** (no code written yet). Produced by a code-grounded,
> adversarially-reviewed planning pass (4 plan drafts → 3 judge panels → synthesis → 5
> verification refutations folded in as mandatory corrections). Mirrors PLAN-0.4.md's
> Factorio-cadence milestone style. Commit/release only on explicit request.
>
> **What changed after verification:** three of the winning proposal's headline claims were
> refuted against the actual code and are corrected below — (1) the "reuse the 0.4 finisher,
> just make it radial, zero new machinery" claim (the shipped finisher is a **world-Y column**
> solver; radializing it is a real rewrite); (2) the "keep the slab, slab count unchanged,
> RAM drops 23.8→6.7 MiB" claim (impossible while `voxels[]` is inline in `Chunk` and the pool
> is one contiguous `calloc` — the win requires `voxels` to become a **pointer**); (3) the
> "0.00002 vox float error" number (the real worst-case is **~0.22 vox at R=512**, a ~2× margin,
> not 4 orders of magnitude). The architecture below is built on the corrected facts.

## 1. Headline

**Make water finite, and make it flow.** 0.4 lit the heat CA world-wide (forge metallurgy).
0.5 ships the soul of the release: **finite water that flows downhill under RADIAL gravity,
pools in basins, and settles to a clean spherical-shell surface** — rivers from a spring or a
placed source, on a planet that is finally a real size at the new fine grain. The planet
starts **dry**; every drop comes from a source and is conserved. The full water *cycle*
(evaporation / cloud / snow / ice / melt) stays **deferred to 0.6** — 0.5 is finite water from
a source, flowing and pooling, thermally inert (no freeze/boil yet).

This release also lays the **foundation slice** the fine-voxel future needs: a metres↔voxels
split so physical constants stop being implicit, a properly-sized planet, and sparse storage
so 72%-air resident windows stop costing 16 KiB a chunk.

## 2. The load-bearing decisions (RESOLVED — design within these)

| Decision | Resolution | Why |
|---|---|---|
| **Grain** | **Compile-time** `k=2` (0.5 m/voxel), one `#define VOX_GRAIN_MM=500` in a new `src/units.h`. `CHUNK_DIM` stays 16. | Every integer hot loop (greedy mesher, heat CA, water CA) is byte-unchanged; the von Neumann 1/6 stability proof is preserved. The metres↔voxels split lives only at the **physics/worldgen/render boundary**, all constant-folded. A future 0.25 m experiment is a one-line change behind the same macros. |
| **Planet size** | **R = 512 voxels = 256 m** physical radius (was 64 vox = a 32 m pebble at the new grain). Center kept **near the world origin** at `(8, R+pad, 8)`. | 512 keeps gentle curvature and a ~1.6 km circumference to walk, and is the **safe radius** for float32 (below): small absolute coordinates keep the ULP tiny. It is deliberately **not** R=1000 (the largest candidate — most worldgen surface, worst dam-break active-cell pressure on a single-core Pentium M). |
| **Float precision** | **Keep float32** in `player.c` — no doubles, no recentred origin. Documented escape hatch + `_Static_assert` pin R inside the float32 safe envelope. | The conclusion is right but the winner's *number* was wrong: measured worst-case lateral `up`-error at R=512 is **~0.22 vox (~110 mm)**, a **~2×** margin under the 0.45-vox collision tolerance — not "0.00002 vox / 4 orders of magnitude." Real but adequate. Made a **regression assert**, not a silent bet. |
| **View policy** | **Hold the resident window ~1521 chunks**; the finer grain shrinks the physical view radius 96 m → 48 m, fog-masked. Full-96 m view is a **runtime tunable** (`g_view_radius`, opt-in), funded by sparse storage. | Default ships at WORLD_RADIUS=6. The band becomes a **surface-following 9-layer shell** re-anchored per-frame (new streaming work — see §5.2). |
| **Sparse-air storage** | **`Chunk.voxels` becomes a `Voxel*`** drawn from a separate bounded slab sub-pool. Uniform-**air** chunks hold `voxels == NULL` + a `uniform_word`; only non-uniform chunks borrow a 16 KiB slab. | **Corrected:** keeping the inline array "in the pool, slab count unchanged" saves **zero** bytes — the pool is one contiguous `calloc(1777, sizeof(Chunk))` (27.8 MiB) committed at init. The pointer split is the **only** design that drops resident voxel RAM (~28% non-air × 16 KiB ≈ **~6.8 MiB**). Uniform-**solid** is **out** for 0.5 (its neighbour-read hazard is unsafe — see §5.1). |
| **Binary fill** | Water fill is **{0,15}** (`EMPTY`/`WATER`), implemented as the existing fill nibble's `1-bit-present` interpretation. The u32 voxel layout is **byte-identical**. | The 0..15 partial-fill prototype (`proto/`, `src/proto_water.c`) **proved** a local head field never settles. Binary fill removes the gradient to chase, so the lateral pass is a terminating occupancy spread. The fill nibble is **reused, not added** — `PERSIST_VOX_MASK` is unchanged. |
| **Surface levelling** | **Two-layer:** (1) a provably-terminating **terraced settle** as the correctness baseline (Lyapunov-monotone flow rule, `active→0` guaranteed); (2) a **gated, bounded, disable-able** radial shell-snap finisher on top for the clean surface. | **Corrected:** the radial finisher is **genuine new machinery** (a from-scratch shell solver replacing the Y-column solver — see §5.3), so it is given its own milestone and its own off-switch. A snap regression cannot block the release: the game still ships with conserving (if terraced) lakes. |
| **Heat coupling** | **Defer** freeze/boil to 0.6. Water shares the 0.4 `SimState`/active-front/READ-COMMIT seam but is **thermally inert** in 0.5. `heat[src]→heat[dst]` is advected on each water move anyway (nearly free) to pre-wire the 0.6 coupling. | Matches the brief's 0.6 water-cycle line; keeps the diff minimal. |
| **Compatibility** | **One hard break.** `WG_GEN_VERSION 2→3`, `PERSIST_FORMAT_VERSION 1→2`, `NET_PROTOCOL_VERSION 3→4`, all at the single grain-flip milestone. 0.4 saves are **refused**, not migrated. | A 0.4 save is a 32 m pebble of 1 m voxels with no water; migrating it to a 256 m planet of 0.5 m voxels is meaningless. Refusal is honest. One break point, clean version-bump refusals everywhere else. |

## 3. What the code-grounding overturned (verified facts)

| Assumption (from the proposal) | Reality (verified against the code) |
|---|---|
| "A uniform chunk keeps its 16 KiB slab in the pool, slab count unchanged, **and** RAM drops 23.8→6.7 MiB." | **Internally impossible.** `chunk.h:35` declares `Voxel voxels[CHUNK_VOXELS]` **inline**; `world.c:542` is one `calloc(WORLD_POOL_SLOTS, sizeof(Chunk))`. Every slab is committed at `world_init`. Not writing `voxels[]` reclaims nothing. The RAM win **requires** `voxels` to become a pointer + a separate lazily-handed-out slab sub-pool. |
| "`WORLD_POOL_SLOTS = 804`." | **It is 1777.** `(2·6+1)² · 9 + 256 = 1521 + 256 = 1777` (≈27.8 MiB dense). The "804"/"402"/"338" in `world.h`'s comments is **stale text** from a smaller milestone; the macro evaluates to 1777. The pool is **larger** than the 1521 window, not smaller — so it *can* back the window densely, but at 27.8 MiB. |
| "Reuse the 0.4 finisher, only change: order cells by radius. No new machinery." | **A rewrite.** `flood_body`/`flat_write_body` (`sim.c:1258–1422`) group body cells into `(lx,lz)` **columns** (`g_col_lx/lz`), then **binary-search a single scalar world-Y surface level `S`** with `cv = S - ly·FLUID_FULL`. That is an iso-Y *plane*. A spherical *shell* has no `(lx,lz)` column to search. `surf_of`, `col_surf`, `head_relax`, `free_surface`, `sim_liquid_unsettled` are all `±Y`-hardcoded (`sim.c:740–924`). Side-of-sphere water (where "down" is ±X/±Z) is **misclassified** by the Y-only predicates. |
| "`normalize(pos-center)` float error is 0.00002 vox." | **Off by ~10,000×.** The dominant error is the float32 **quantization of `pos`** (ULP ~6e-5 vox at world-Y ~792), not subtraction cancellation. Measured worst-case lateral `up`-error at R=512 ≈ **0.22 vox**. Conclusion (float32 OK) holds; the **margin is ~2×, not 4 orders**. |
| "`world_pin`/`world_unpin` reuse." | **They do not exist** in `world.c`/`world.h`. The brief forbids inventing APIs and claiming reuse. 0.4's g_xsim eviction-survival is by **coord re-check** after streaming (`main.c:2152`). 0.5's pin is **new code**, scoped as such (§5.4). |
| "Grain split lives ONLY at player_defaults + worldgen relief." | **Incomplete.** Missed: `BLOCK_REACH=6.0f` ("in voxels", `main.c:402`, fed to `raycast` — edit reach would silently halve 6 m→3 m); the spawn offset `+6.0f` (`main.c:1588`); `eye=1.62`/`height=1.8`/`half_xz=0.3`/`swim_vel=4.0`/`water_term=3.0` (`player.c:120–130`, all voxel-unit); shader `FOG_END=340.0` (`render.c:299`, voxel-depth — 340 vox·0.5 m = 170 m, **not** the claimed 48 m); `WG_NOISE_PERIOD=32`, `WG_SEA_LEVEL=14` (`worldgen.h`). The split must be an **audited grep gate**, not a hand-enumerated list. |
| — | **Confirmed good:** `MAT_WATER=3` already exists (`material.h:91`); `MAT_AIR=0` so `calloc`-zero = valid all-air; `sim_set_spring`/`is_spring` already wired (`sim.h:704`); PHASE 1.6 fluid pass already runs after the heat commit; `neigh[6]` cross-chunk cache wired; `sim_tick_ex` READ/COMMIT seam shipped; `R2` at R=512 = 786432 (fits int32 with vast headroom, but `worldgen.c:183` uses `long` d2 → 32-bit on MinGW LP32, so a guard is cheap insurance). |

## 4. Architecture — the 6 open problems

### 4.1 Grain plumbing (metres↔voxels)
New header **`src/units.h`** (additive, pure macros, constant-folded):
```
#define VOX_GRAIN_MM        500          /* 0.5 m/voxel — 0.5 SHIPS k=2 */
#define METRES_PER_VOXEL    (VOX_GRAIN_MM / 1000.0f)
#define VOXELS_PER_METRE    (1000.0f / VOX_GRAIN_MM)   /* = 2 */
#define M2V(m)  ((m) * VOXELS_PER_METRE)   /* metres  -> voxels */
#define V2M(v)  ((v) * METRES_PER_VOXEL)   /* voxels  -> metres */
_Static_assert(1000 % VOX_GRAIN_MM == 0, "grain must divide 1 m");
```
**Rule (folded from P1/P2/P3):** physical *lengths and speeds* are authored in **metres** and
wrapped in `M2V` so they preserve their physical magnitude across grain. **Collision-grid
geometry** (`r_p`, `max_substep`) is authored the same way (`M2V(0.45m) → 0.9 vox`, so the body
stays 0.9 m wide). At `k=2` every converted constant is exactly **2× its 0.4 literal**, so the
feel is preserved by construction. The full converted set (NOT just the proposal's six):

- `player_defaults()`: `half_xz=M2V(0.3)`, `height=M2V(1.8)`, `eye=M2V(1.62)`, `gravity=M2V(28)`,
  `term_fall=M2V(56)`, `walk_speed=M2V(4.3)`, `jump_vel=M2V(8.4)`, `swim_vel=M2V(4.0)`,
  `water_term=M2V(3.0)`, `r_p=M2V(0.45)`, `max_substep=M2V(0.45)`.
- `main.c`: `BLOCK_REACH = M2V(3.0)` (3 m reach), spawn offset `+ M2V(3.0f)`.
- `render.c`: `FOG_END`/`FOG_START` passed as **uniforms** computed from the physical view
  radius (`V2M`), reconciled so fog ends near 48 m (96 vox) on the default window — fixing the
  proposal's self-contradiction (340 vox ≠ 48 m).
- `worldgen.h`: `WG_PLANET_R`, `WG_HEIGHT_AMP`, `WG_DIRT_DEPTH`, `WG_SEA_LEVEL`, and a
  deliberate decision on `WG_NOISE_PERIOD` (hold the terrain *wavelength* in metres → `M2V(16m)=32`).

The hot loops (`sim.c`, `mesher.c`) index in **integer voxels exactly as today** — zero runtime
cost, no shape change. **Enforcement:** M2 adds a `make check` grep gate that scans `player.c`/
`main.c`/`render.c`/`worldgen.h` for bare world-space float literals in unit-bearing fields and
asserts each documented physical constant round-trips through `M2V`/`V2M`. The boundary is
*audited*, not enumerated by hand.

### 4.2 Sparse/uniform-air storage (the real RAM win)
**`Chunk.voxels` changes from an inline `Voxel voxels[CHUNK_VOXELS]` to a `Voxel *voxels`**,
backed by a **separate bounded slab sub-pool** of bare 16 KiB voxel blocks (sized to the measured
~28% non-air working set + churn slack, e.g. ~512–640 blocks ≈ 8–10 MiB). The resident `Chunk`
record (coords/`neigh`/flags/`active_count`/`uniform_word`) is ~88 B and always resident.

- **Uniform-AIR only** for 0.5. A uniform chunk has `voxels == NULL`, `flags |= CHUNK_UNIFORM`
  (`0x10`), and `uniform_word` = the canonical air word. Uniform-**solid** is explicitly **out**:
  it would be read as air through the neighbour-pointer dereferences that have **no uniform
  guard** (`light.c:105/167/210`, `mesher.c:117–139`, `worldca_nfn` `main.c:608`), making the
  mesher emit interior faces and letting water flow through bedrock. Air reads correctly as air
  (zero word), so uniform-air is safe; uniform-solid is a 0.6 opt after the readers are routed.
- **Worldgen short-circuit:** `worldgen_fill_chunk` gets a per-chunk min/max radius pre-test
  (the corner-`d2` extremes vs `R2`/`Rd2`). A wholly-exterior chunk is tagged uniform-air with
  **no slab popped, no voxels touched**. 72% of the window is pure air → 72% skip the slab, the
  mesh, and the CA entirely (zero active voxels — the CA's existing "uniform chunk costs nothing"
  invariant).
- **Copy-on-write `chunk_realize(Chunk*)`:** on first edit or first water arrival, pop a slab
  block from the sub-pool, expand `uniform_word` into all 4096 voxels, point `voxels` at it,
  clear `CHUNK_UNIFORM`. The block is stable for the chunk's residency (so `s->chunk->voxels` and
  `neigh[]` reads stay valid — the COW pointer-stability property). Eviction returns the block.
- **All readers route through a `chunk_uniform_word(const Chunk*)` accessor** (returns
  `uniform_word` if `CHUNK_UNIFORM`, else `voxels[...]`) at every `neigh[]` read site and in
  `persist`/`net` encode. **Persist/net encode from `uniform_word`** when `CHUNK_UNIFORM` is set
  (one palette entry + one run) — never dereference NULL `voxels`. The CA must `chunk_realize()`
  **before** `sim_init` seeds `heat[]` from `voxels[]`.

### 4.3 The binary-fill water CA
- **Flow rule (validated, ~46 ns/active-voxel):** GRAVITY FIRST — a water voxel whose radial-down
  neighbour is empty moves wholesale into it (full transfer, conservative via the existing
  `moved_mask`). Then LATERAL SPREAD — if down is blocked, donate to an empty equal-radius
  neighbour, parity-alternating visit order keyed on `tick_index` (the existing even/odd X/Z swap)
  for bias-free determinism. Binary fill ⇒ no 0..15 gradient ⇒ a terminating occupancy spread.
  **Lyapunov termination (folded from P3):** the sum of per-voxel radial heights is monotone
  non-increasing under the rule, so the front provably reaches `active==0`. This is the
  **correctness baseline** and it ships first (M3), independent of the finisher.
- **Radial "down" (folded from P4 — the perf win the bigger R buys):** the dominant down-axis is
  cached **per chunk-center, once per tick** (a 16-voxel chunk subtends <2° at R=512, so the whole
  chunk's down is one integer `argmax` over the 6 face d2-deltas `2·(coord−C)±1` — no sqrt, no
  per-voxel pick). Cheaper than a per-voxel pick on the Pentium M. **Boundary caveat (flagged):**
  a voxel near a chunk corner on a steep shell can get a down-axis one face off its own per-voxel
  radial; the **M3 far-hemisphere test must check this explicitly**. The finisher uses the **exact
  per-voxel `d2`** for the shell test, so levelling stays precise even where the per-tick flow
  quantizes at chunk granularity.
- **Finite water + sources:** water is conserved by construction (sum of WATER voxels invariant
  except at sources). A **spring** reuses the shipped `sim_set_spring()`/`is_spring` (re-stamped
  to full each tick). 0.5 ships a **placed `MAT_WATER_SOURCE`** (registered as a spring on
  placement) and an optional worldgen **spring marker** near a peak. The planet starts dry. A cell
  drained to fill==0 reverts to `MAT_AIR` (the sim already does this for liquids). Non-spring
  water is strictly conserved → no infinite-water bug.
- **Heat coexistence:** water rides the SAME `sim_tick_ex` READ/COMMIT seam and per-chunk active
  list as heat; PHASE 1.6 already runs after the heat commit. Heat (`heat[]` double-buffered) and
  water (occupancy moved in-place via `moved_mask`) touch disjoint fields. Freeze/boil is **out**
  (0.6); `heat[]` is advected on each move to pre-wire it.

### 4.4 The radial surface-levelling finisher (NEW machinery — not a one-line reuse)
The shipped finisher cannot be radialized by reordering. The **flow rule's terraced settle is the
shipped surface; the finisher is layered on top, gated and disable-able.** What it actually
requires:
- **Replace the `(lx,lz)`-column model with RADIAL BINS.** A connected-body flood (reusing the
  `g_body`/`g_seen` BFS scaffold that *does* reuse cleanly) buckets each body cell by integer
  `d2 = dx²+dy²+dz²` (the worldgen idiom). Fill bins from **smallest-d2 upward** until the body's
  conserved water count `N` is exhausted, writing the partial "surface" bin to satisfy `N` exactly.
  This is a true order-by-radius-and-fill that conserves `N` — what the proposal *described* but
  the shipped code does **not** do (it solves a scalar world-Y level).
- **Re-derive the sleep guard radially.** `sim_liquid_unsettled`/`free_surface` use the per-voxel
  radial-down face instead of `±Y`, so side-of-sphere water classifies correctly and the body
  does not latch a wrong shape. **Anti-re-fire guard:** after a snap, record the body's radial
  equilibrium in `fluid_fired` AND suppress the local rule on cells whose only move stays within
  the snapped shell, so the snap is the rule's **fixed point** (water is period-1, so without this
  a genuinely stuck pool recurs every tick and re-fires — the design's own risk #1).
- **Multi-chunk.** Any real lake spans chunks; a per-chunk Y-snap staircases at seams permanently.
  The connected-body flood operates across the WorldCA active-chunk set over the `neigh[6]` seam,
  gated behind the same READ/COMMIT seam, pinning the body's chunks during the snap.
- **Gated + bounded + off-switch (folded from P3 — de-risks P4's "ship blocker" bet):**
  `VOXEL_FLUID_SNAP_MAX` caps the body size the snap will fire on; a config flag disables it
  entirely. If the snap pops or busts the frame budget on a large sea, the engine still ships with
  the **proven-terminating terraced baseline**. The snap is a *fidelity layer over a correctness
  floor*, not the floor itself.

### 4.5 Integration
- **Cross-chunk flow** reuses 0.4's `neigh[6]` cache and the exact `sim_tick_ex` READ-before-COMMIT
  seam (a seam-crossing water cell reads the neighbour's start-of-tick state; order-independent).
  A cell crossing a seam **wakes the neighbour chunk enqueue-only** via `worldca_wfn`.
- **Conservation across async-evicting faces** (the hazard 0.4 explicitly deferred): a **new**
  `world_pin`/`world_unpin` API (refcount on the slab + an eviction guard in `world_evict`) pins
  active water chunks + their 6 neighbours so no in-flight mass is lost. **Reconciled against the
  REAL pool (1777 slots, not 804):** with `WORLDCA_MAX_XSIMS=48` active chunks, the worst pin set
  is 48·7 = 336 ≪ 1777, so pinning cannot deadlock streaming — proven by an M4 test that drives
  48 active water chunks and asserts the pin set never starves the leading curtain.
- **Light re-bake** when water carves/drains a cave: the existing `CHUNK_DIRTY_MESH` →
  `light_compute` → `greedy_mesh` path, unchanged.
- **Streaming:** host pushes `CHUNK_MODIFIED_BY_SIM` water chunks via the existing `MSG_CDELTA`
  cadence (clients run zero CA). The wire payload is **RLE'd** (binary fill compresses to runs of
  full/empty). **Wire budget gate (folded from P3):** a full-water chunk delta is ~24576 B, under
  `NET_CHUNK_MAX=28672` (`net.h:37`) — the streaming milestone asserts it stays single-frame and
  **forbids adding a per-voxel flow byte** to the wire entry without fragmentation.

### 4.6 Compatibility (one hard break)
| Version stamp | 0.4 | 0.5 | When | Rationale |
|---|---|---|---|---|
| `WG_GEN_VERSION` | 2 | **3** | M2 (grain flip) | Grain k=2 changes worldgen output (R, center, strata, noise wavelength). 0.4 chunks regenerate differently → refused, exactly the gen-mismatch refusal `worldgen.h` already documents. |
| `PERSIST_FORMAT_VERSION` | 1 | **2** | M2 | Fill semantics narrow to binary {0,15} for water; the version bump cleanly refuses 0.4 saves rather than mis-reading them. Codec machinery (palette+RLE) is reused unchanged. |
| `NET_PROTOCOL_VERSION` | 3 | **4** | M5 (water stream) | A 0.4 host/client pair cleanly refuse each other at the handshake. |

**Single break point at M2.** `CHUNK_DIM` stays 16; the u32 voxel layout is byte-identical (only
fill *semantics* narrow), so the GPU/mesh format and persist codec are reused. Hard break, not
migrate: a 0.4 32 m-pebble save cannot meaningfully become a 256 m planet.

## 5. The named hazards each milestone retires (cross-references §4)

1. **Sparse RAM is a pointer refactor, not a 2-field diff** → M1 makes `voxels` a pointer and
   proves the real ~6.8 MiB drop, in isolation, before any grain or water change.
2. **Surface-following band re-anchor is new streaming behaviour** (`WORLD_BAND_Y1` is fixed
   today) → M2 lands it with a fall-terminal-velocity (`M2V(56)` = 112 vox/s) lead margin so a
   fast-falling player can't outrun the band into a hole.
3. **The radial finisher is a rewrite** → M4 is its own milestone, layered over M3's proven
   terraced baseline, gated and disable-able.
4. **Pin vs the real 1777-slot pool** → M4 proves 48·7 pins ≪ pool.
5. **Combined heat+water worst case shares the 66.7 ms tick** → the dam-break frame-budget probe
   runs **at M3** (when the fixed-point guarantee that bounds cost is established), not deferred to
   M5; M5 re-runs it concurrent with a heat forge across the 48-chunk cap as the release gate.

## 6. Build order (Factorio cadence — every boundary builds, runs, releases; both targets `-Wall -Wextra` clean; full suite green via `make check`)

| ID | Goal | `make check` verification | Save/byte-compatible with prev? |
|---|---|---|---|
| **M0** | Branch `0.5.0-dev`; `version.h`; CHANGELOG stub. Add **`src/units.h`** (`VOX_GRAIN_MM`/`M2V`/`V2M`) — **pure additive header, applied nowhere yet.** Extend `make check` with empty `test_grain`/`test_water` stubs + a GL-free water determinism harness slot (`#ifdef VOXEL_DETERMINISM_HARNESS`, reusing `sim_state_hash`, which already covers `fill`). | `make check` green; binary **byte-identical to 0.4.0** except the version string. | **YES** |
| **M1** | **Sparse storage as a pointer refactor.** `Chunk.voxels` → `Voxel*`; new bounded slab sub-pool; `CHUNK_UNIFORM` flag + `uniform_word`; `chunk_uniform_word()` accessor routed through **all** `neigh[]` readers (`light.c`, `mesher.c`, `worldca_nfn`) + persist/net encode; worldgen min/max-radius short-circuit; `chunk_realize()` COW. Still **1 m grain, still R=64** — pure storage refactor, verifiable in isolation. | `test_sparse.c`: a uniform-air chunk has NULL `voxels`, meshes to empty, CAs to zero active, realizes byte-identically to a dense fill on edit; **resident voxel RAM drops 27.8 → ~6.8 MiB (measured)**; persist + net round-trip of a uniform chunk is byte-identical (encoded from `uniform_word`); a `neigh[]` read of a uniform chunk returns air via the accessor (no NULL deref). 0.4 saves still load (R/grain unchanged). | **YES** (on-disk format & behaviour unchanged; `sizeof(Chunk)` shrinks but persist serialises canonical words, not the struct) |
| **M2** | **Grain flip + planet resize + audited boundary.** `VOX_GRAIN_MM=500`; re-express the **full** constant set (§4.1) via `M2V`; `WG_PLANET_R=512`, center `(8, 540, 8)`, `WORLD_BAND` → surface-following 9-layer shell with a fall-velocity lead margin; `FOG` uniforms from `V2M`; `_Static_assert(3*(int64_t)WG_PLANET_R*WG_PLANET_R < INT32_MAX)` on the worldgen d2. Bump `WG_GEN_VERSION 2→3`, `PERSIST_FORMAT_VERSION 1→2`. **No water yet** — verify the 256 m sphere is walkable. | `test_grain.c`: `M2V`/`V2M` fold correctly and the physical body is preserved (`r_p`·METRES_PER_VOXEL = 0.45 m); the **grep gate** asserts every unit-bearing literal routes through `M2V`/`V2M`; the float-precision regression asserts worst-case `up`-error **< 0.25 vox** at R=512 (margin tracked, not assumed); manual `/run` walkaround; resident count stays ~1521; band lead beats `M2V(56)`. **HARD BREAK:** 0.4 saves refused at the gen/format mismatch. | **NO** (the one unavoidable break) |
| **M3** | **Binary-fill water — flow rule + terraced baseline (the correctness floor).** Restrict water fill to {0,15}; per-chunk-center cached radial-down replacing fixed `-Y` in `fluid_step`; `MAT_WATER_SOURCE` placeable + `sim_set_spring` wiring; planet dry. **Terraced settle only — no finisher yet.** Single-chunk (faces closed) to retire the flow-rule risk first. | `test_water.c`: a pour **settles to `active==0` in ~107 ticks** with water conserved (matches the measured prototype, asserted via the **Lyapunov-monotone** argument); a 4500 m³ dam-break peaks ~15.3k active, **~8.5 ms/tick on the budget model**; water flows toward center from **any face** of the sphere (the far-hemisphere + chunk-corner down-axis test); spring inexhaustible, non-spring water strictly conserved. **Combined heat-forge + water dam-break frame-budget probe is an M3 gate.** | **NO** (depends on M2's grain) |
| **M4** ✅ | **Cross-chunk flow (SHIPPED).** ~~Radial shell-snap finisher~~ **→ deferred to 0.6** (binary fine voxels settle natively; the river-focused foundation does not need flat lake surfaces — see CHANGELOG/M4 design note) and ~~`world_pin`~~ **→ deferred to 0.6** (the deposit apply is already crash-safe via residency re-checks; pin only avoids losing in-flight water on stream-away). **DELIVERED:** open water across chunk faces over the `sim_tick_ex` seam via a deferred **atomic deposit** on the WorldCA's READ/COMMIT boundary (`SimXFlowFn` on `SimState`; `worldca_xfn` + a post-commit deposit-apply queue in `main.c`) — deterministic (enqueue order, first-wins corner conflicts), conserved (materialise neighbour + revert source), realizing a uniform-air neighbour first. | `test_water.c` cross-chunk seam case: a boundary voxel **enqueues → crosses the −Y seam → vacates source → conserves** total water; `fluid_xfn==NULL` keeps single-chunk walls closed (M3 tests unchanged). make check green; both targets clean; ASan/UBSan clean over a forge+water+fly session. | **YES** (cross-chunk flow additive; save/wire formats untouched until M5) |
| **M5** ✅ | **Host-authoritative water-delta streaming (SHIPPED).** The host-authoritative substrate already existed (0.4 M5: WorldCA host/SP-only, clients run zero CA); the full voxel word already streams so the water `fill` rides it for free + M3/M4 already flag `CHUNK_MODIFIED_BY_SIM`. The genuine M5 work was the **bandwidth**: **RLE** the chunk-delta — `chunksync.c` coalesces index-consecutive same-canon voxels into `(u16 start, u16 len, u32 voxel)` runs, so a flooded 4096-voxel chunk drops **24 KB → 22 B**. `NET_PROTOCOL_VERSION 3→4`; `NET_CHUNK_MAX 28672→36864` sized for the RLE worst case (4096 single runs = 32782 B, never truncates). | **`test_water_net.c`** (NEW): two-peer **loopback** water render-fidelity — host floods a chunk, serves via the real RLE codec, pushes over the real socket, client reconstructs water voxel-for-voxel (delta ≤64 B); a 4-layer pool transfers exactly. `test_chunksync.c`: flood→1 run (22 B) + worst-case 4096-run no-truncation. Adversarial 3-lens review: **0 confirmed defects**. (0.4↔0.5 already refused by M2's `gen_version`.) | **YES** (net handshake gated cleanly; save format unchanged from M2) |
| **M6** | **Release hardening & ship 0.5.0 (only when asked).** Both targets warning-clean; version-string sweep; **win-target water-determinism under wine**; the worst-case dam-break **concurrent with a heat forge** holds 30 fps on the budget model (the real combined gate); release binary has no `sim_state_hash`. Then the ritual: drop `-dev`, CHANGELOG **loudly stating the WG/persist/net hard break + "0.4 saves are refused, not migrated"**, `make archive`, tag, push. | `make check` green incl. the combined frame-budget probe and the wine determinism run; fixture asserts a 0.4 save is **refused** (not mis-read). | **YES** (no format change at M6) |

**Sequencing rationale (risk front-loaded):** storage (M1, byte-compatible, R unchanged) → grain
flip (M2, the one hard break, with float precision retired *here* at the shipping R) → single-chunk
water flow + the proven terraced floor (M3) → the radial finisher *as a separate, gated layer*
+ cross-chunk + pin (M4) → streaming (M5) → hardening (M6). The two genuinely new pieces (the
pointer-based sparse storage and the radial finisher) each get their own isolated, independently
verifiable milestone *before* the things that depend on them. The float-precision and combined
frame-budget unknowns are retired by **measurement**, not hope.

## 7. RISKS (acknowledged, with mitigations)

1. **The radial finisher reading as "settling" vs "popping/teleporting"** is the headline fidelity
   bet — only a visual test can clear it. *Mitigation:* it is layered over M3's proven terraced
   baseline and is `VOXEL_FLUID_SNAP_MAX`-bounded and **disable-able**, so a snap regression ships
   conserving (if terraced) lakes rather than blocking the release. The anti-re-fire guard makes
   the snapped shell the local rule's fixed point so water (period-1) cannot re-fire it every tick.
2. **The Y-column→radial-shell rewrite is real work**, not a reorder — `flat_write_body`/
   `flood_body`/`surf_of`/`col_surf`/`head_relax`/`sim_liquid_unsettled` are replaced/re-derived.
   *Mitigation:* M4 budgets for it as new machinery; only the `g_body` BFS scaffold and the
   limit-cycle ring reuse cleanly.
3. **Per-chunk cached down-axis quantizes at chunk corners on a steep shell** (a voxel near an edge
   can get a neighbour's down). *Mitigation:* the M3 far-hemisphere + chunk-corner test checks it
   explicitly; the finisher's per-voxel `d2` is consistent, so levelling stays precise even where
   per-tick flow biases by one axis.
4. **Combined heat+water on the single-core Pentium M shares the 66.7 ms tick.** The ~8.5 ms
   dam-break is the flow rule in isolation; running it concurrent with a heat forge across 48
   active chunks is the real gate. *Mitigation:* the combined probe runs at M3 and is the M6
   release gate; if it busts, the water active-cap is lowered (the cap is a tunable, not a constant).
5. **Pin vs the 1777-slot pool** — pinning 48 active water chunks + 6 neighbours each. *Mitigation:*
   48·7 = 336 ≪ 1777; the M4 test drives the worst case and asserts no streaming starvation.
6. **Surface-following band re-anchor** is new streaming behaviour (`WORLD_BAND_Y1` is fixed today).
   *Mitigation:* a band lead margin > fall terminal velocity (`M2V(56)` = 112 vox/s), tested in M2.
7. **Float32 margin is ~2×, not "4 orders."** *Mitigation:* it is a tracked regression assert
   (< 0.25 vox at R=512), and the center-near-origin choice + `_Static_assert` keep R inside the
   safe envelope. **Escape hatch:** a future >~2000-vox planet (0.7+) would compute `up` in double
   in `player.c` (one off-hot-path vector/frame, never touching the integer CA) — the decision is a
   *bounded* one-way bet, documented, not silent. The x87-vs-SSE2 MinGW portability surface that
   doubles would add is **avoided** in 0.5 because float32 suffices at R=512.
8. **Binary fill drains in whole-0.5 m steps** (no thin film). Accepted within the decided
   binary-fill constraint; an explicit 0.6 render-only sub-voxel-surface note.
9. **Uniform-solid is unsafe today** (neighbour readers have no uniform guard). Deferred to 0.6
   after the readers are routed through the accessor for both cases; 0.5 ships uniform-air only.

## 8. Measured evidence (probe numbers this plan is built on)

- 1 m grain, real engine: resident window **1521 chunks** (72% pure air); voxel RAM **23.8 MiB**;
  `sizeof(Chunk)` = **16.1 KiB**; `sizeof(SimState)` = **89.5 KiB** per active chunk; worldgen
  **0.006 ms/chunk**; opaque mesh **0.15 ms/chunk** (272k verts → 3.9 MiB GPU mesh); heat CA
  **83.5 ns/voxel-tick**.
- Binary-fill water CA prototype: **~46 ns/active-voxel**; a 4500 m³ dam-break peaks **~15.3k
  active cells (~8.5 ms/tick on target)** — bounded, cheap; a finite pour **settles to active==0
  in ~107 ticks**, water conserved.
- The simple gravity + flow-to-descent rule does **not** level a flat still surface (water
  terraces) — *the* open fluid problem, which is why the finisher (radialized, gated) exists.
- Verified-against-code corrections: `WORLD_POOL_SLOTS` = **1777** (not 804; ≈27.8 MiB dense);
  the finisher is a **world-Y column** solver (`sim.c:1258–1422`); `world_pin`/`unpin` **do not
  exist**; float32 worst-case `up`-error at R=512 ≈ **0.22 vox** (~2× margin, not 0.00002);
  `R2`(512)=786432 fits int32 but `worldgen.c:183` d2 is `long` (LP32 hazard → `_Static_assert`);
  `BLOCK_REACH`/`FOG_END`/`WG_NOISE_PERIOD`/`eye`/`half_xz` are voxel-unit and were missing from
  the grain split.

## 9. Deferred to 0.6 (correctly out of 0.5)
The full water **cycle** (evaporation / cloud / snow / ice / melt); water↔heat phase coupling
(freeze below 0 °C, boil on lava contact — `heat[]` is advected now to pre-wire it); uniform-SOLID
sparse chunks (after neighbour readers are routed through the accessor); sub-voxel water surface
rendering (thin films); the 0.25 m grain experiment (a one-line `VOX_GRAIN_MM` change behind the
same macros); a recentred-origin / double-precision `up` frame (only if a km-scale planet exceeds
the float32 safe envelope).
