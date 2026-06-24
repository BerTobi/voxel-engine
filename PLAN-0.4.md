# Voxel Engine 0.4 — "The World Comes Alive" — Implementation Plan

> Status: **DRAFT for sign-off** (no code written yet). Produced by a code-grounded,
> adversarially-reviewed planning pass (6 subsystem readers → 3 plan drafts → synthesis →
> 4 review lenses → revision). Stays on the **1m uniform grid** (the fine-voxel shrink is a
> deferred 0.5 thing). Commit/release only on explicit request.

## 1. Headline

Unfreeze the cellular automaton. Today it ticks **one inert demo chunk** and is dead on the
sphere. 0.4 makes **heat diffusion + melting/freezing tick world-wide across every resident
chunk, with heat bleeding across chunk faces** — lighting the forge/metallurgy discovery loop
`progress.c` was built for (trap heat in a stone cavity, melt copper ~1085 °C, climb toward
iron ~1538 °C; capability *emerges* from sustained temperature, no tech tree), and surfacing
it in an **in-world journal HUD**.

## 2. The one load-bearing decision — multiplayer-CA determinism

**Resolved: host-authoritative sim streaming (Option B) for 0.4. Deterministic lockstep
(Option A) deferred to 0.5.**

- **Why B:** clients *already* run zero CA (verified `main.c:1727`), and host-auth has **no
  cross-machine arithmetic dependency** — one authority, nothing to keep bit-identical. It
  reuses the existing chunksync wire shape (host pushes CA-changed chunks as `MSG_CDELTA`),
  cannot regress 0.3 (edits/poses unchanged, deltas additive), and a stale 0.3 client is
  cleanly refused at the version handshake (`NET_PROTOCOL_VERSION` 2→3).
- **Honest caveat (folded from review):** everything crossing a boundary (wire, late-join,
  persist) is the **lossy 8-bit temp code**, while the authoritative state is full-resolution
  `heat[]` (1/64 °C) + `latent[]`. So the M5 test verifies **render-fidelity** (faithful 8-bit
  receipt), *not* sim-state-equality. This is fine *only because clients are passive*. It is a
  **0.5-blocking gap for lockstep**, which additionally needs (1) full-resolution `heat[]`+
  `latent[]` in the payload (or a proof that re-seeding from the 8-bit code is a fixed point)
  and (2) ~~tick-stamped edits~~ — **(2) is now pulled into 0.4 (see M3a) by decision**, closing
  it early. 0.4 builds the correct *substrate* (global logical tick, canonical order,
  residency-lock, global phase split, fixed-ambient boundary, GL-free determinism harness,
  **tick-stamped edits**) — so 0.5 lockstep is "incremental **plus** gap (1) only," not a free
  flip.

## 3. What the code-grounding overturned (verified facts)

| Assumption | Reality (verified) |
|---|---|
| Forge demo can live in HOME chunk | HOME `(0,1,0)` = world-Y 16–31, **~40 voxels inside the solid stone core**; player spawns at world-Y **134** (`main.c:1202`, chunk cy=8, surface). A forge in HOME is **unreachable**. `demo_decorate` is dead-coded (`main.c:487-489`) and its header is stale flat-world text. |
| "Sync sim state" | Authoritative state = full-res `heat[]` (1/64 °C) + `latent[]`; voxel word + `MSG_CDELTA` + chunksync + persist carry only the **8-bit temp code** (~1 °C). Re-seed via `temp_to_heat` is **lossy**. |
| CA cost is the worry | The real Pentium-M cliff is **remesh churn**: `dirty_mesh` flips on every ~8 °C code step (`sim.c:1794`) → full `light_compute` BFS + 12 greedy sweeps + 2 GL uploads. A 20→1085 °C ramp is ~130 full remeshes *per voxel*. |
| Active-set is bounded | `SIM_ACTIVE_CAP=4096` is **one chunk's worth** (`sim.h:285`), not a world total. No global cap exists. |
| Sun heats the world | Sun is **render-only** (`main.c:1827` wall-clock `sinf` → light nibble; no sim term). 0.4 must **keep it that way** (a wall-clock float in the CA = instant divergence). |
| — | Sim hot loop is **float-free / fixed-point** ✓. `neigh[6]` cross-chunk cache already wired ✓. `WG_GEN_VERSION=2` is **seed-independent** ✓ (`worldgen.c:168`). Chunk flag `0x04` is free for `CHUNK_MODIFIED_BY_SIM`. No `make check` aggregate exists; connect screen hardcodes `"VOXEL ENGINE 0.3"` (`main.c:709`). |

## 4. Build order (Factorio cadence — every boundary builds, runs, releases; both targets `-Wall -Wextra` clean; full suite green via a new `make check`)

### M0 — Branch, `0.4.0-dev`, `make check` gate, GL-free determinism harness
Branch off master; `version.h` → 0.4.0-dev; CHANGELOG stub. Add **`make check`** (runs every
suite incl. new `testdeterminism`, fails on first non-zero exit) so "full suite green" is
*mechanical*, not honor-system. Build the **GL-free CA driver** (host-CA-tick + delta-push +
client-apply factored out of `main.c` so the unit linker can run two peers in one process —
`VOXEL_HOST` through `main.c` can't serve as the harness, it always creates a GL context) +
an `#ifndef`-guarded `sim_state_hash`. *Ships:* identical to 0.3.0 plus the gate.

### M1 — Reachable **surface** forge (sustained heat + reachable copper), single-chunk
Re-author `demo_decorate` to decorate the **spawn-surface chunk the player actually streams
in** (cy≈8), *not* buried HOME. Default heat = a held `MAT_EMISSIVE` lava/magma pocket
(auto-registered by `sim_init`, **zero new sim code**) + copper ore placed so the player can
wall a stone cavity (stone 250 cW vs copper 40100 = the insulator). Retune `DEMO_WORLD_*`,
camera-aim, the decorate/persist marker. Ore via an **idempotent post-gen pass** on every
chunk bring-up (so a 0.3-saved edited chunk doesn't load ore-less); `WG_GEN_VERSION` stays 2.
*Test:* **end-to-end reachability** (default-spawn camera's window contains the forge) + a
warmup soak melts copper + the stone cavity holds. *No longer the trivial milestone.*

### M2 — In-world journal HUD (parallel with M1)
Frame-local toast (IDLE/SHOWING/FADING) on `prog_observe_drain` count>0 + a toggleable journal
panel, drawn with the 0.3 `render_text`/`render_ui_rect`/`FONT8X8`/`u_alpha` primitives. Draw
fn takes **`const ProgressState*`** (compiler-enforced read-only — the durable guard). HUD is
never sim state. *Test:* WITH-HUD vs WITHOUT-HUD run byte-identical; readonly-invariance green.

### M3a — WorldClock (global logical tick), byte-identical anchor
Introduce one `uint64_t world_tick` owned by `main.c`, advanced by the existing
`sim_accum_ms` (which now only decides *how many* ticks, never *what* a tick computes). Sim
reads the shared clock. `world_tick` **not persisted** (`PERSIST_FORMAT_VERSION` stays 1);
an M3a test proves a reload at a different parity yields byte-identical committed `heat[]`
(heat FTCS has no parity dependence; fluid faces stay closed in 0.4). Low-risk, separately
bisectable. *Caveat stated:* true replay needs the same tick *count* per `world_tick`.
**Edit tick-stamping (pulled in by decision):** stamp every `world_edit_voxel`/`sim_notify_edit`
with the current `world_tick` — a local-only field in 0.4 (host-auth doesn't put it on the wire
yet), closing 0.5-lockstep gap (2) now so that flip is near-free later. Cheap; costs a little
extra surface here.

### M3b — WorldCA (multi-chunk container) — the high-risk refactor
Generalize `SimState` → a `WorldCA` of lazily-allocated, pooled, **hard-capped** per-chunk
`SimChunk` side arrays; tick all resident chunks in **canonical (cy,cz,cx) order** (not
streaming-history order) with **faces still closed** (byte-identical to 0.3 for the forge).
Lands: **hard global active-cell cap** `WORLDCA_ACTIVE_CAP` (cells, canonical-order overflow →
defer); **residency-lock** (`world_pin/unpin`, evict skips pinned active chunks + their 6 face
neighbours, capped against the 804-slot pool); **budgeted sim-remesh queue** (decouple
persist-dirty from mesh-dirty); the **CA-off gate** (`net_mode != NET_CLIENT`) lands *here* so
client passivity is never unguarded; assert client sim-tick counter == 0. *Biggest blast
radius in the plan — keep per-voxel PHASE functions untouched to bound it.*

### M4 — **Open the heat faces** — THE HEADLINE (single-player)
- **Global PHASE split, heat:** all active chunks do PHASE 1 (read start-of-tick `heat[]`,
  queue into a **shared world-scale** `writes[]` buffer) before any PHASE 2 commit.
- **Global PHASE split, transitions (1.5)** — *the hole the original plan missed:* run the
  transition sweep globally after commit, and constrain cross-chunk `wake_ring` to
  **enqueue-only** (no mid-tick neighbour mutation); test a copper voxel melting **on the
  seam**.
- **Open faces:** replace the 6 closed-wall `continue` guards (`sim.c:1641-1644`) with
  neighbour reads via the **`neigh[6]` cache** (no `world_get` in the hot loop), same
  `g_conduct_lut`, von Neumann 1/6 stability preserved. A **non-resident** neighbour reads
  **fixed ambient** (a constant), *not* a closed wall — so the field is independent of where
  the player stands (kills a replay/lockstep position-dependence + is more honest for a cold
  planet).
- **Glow-coalescing mandatory** (`dirty_mesh` only on glow-nibble/mat/fill change, ~16× fewer
  remeshes). **Tick-cost probe** runs here (moved earlier) and gates M5.
- **No sun/solar term in the CA** (explicit guard + regression).
- *Tests:* cross-seam heat appears + conserved; heat order-independence; **melt-on-seam**
  order-independence; **save/evict/reload determinism** (where the `heat[]`/`latent[]` loss
  bites); multi-chunk readonly-invariance (memcmp every active chunk).

### M5 — Host-authoritative sim streaming — THE HARD PROBLEM (multiplayer)
Host, after the CA tick (fixed order: drain edits → CA → push), diffs `CHUNK_MODIFIED_BY_SIM`
chunks against a per-chunk last-sent snapshot (8-bit canon, glow-coalesced), pushes only
changed voxels as `MSG_CDELTA`; **dirty flag clears only after a successful push** (starved
chunks still converge). Per-frame chunk budget + per-chunk coalescing + active-cell-region-only
scan. Clients apply via `world_edit_voxel`, run **zero** CA. Bump `NET_PROTOCOL_VERSION` 2→3.
*Tests:* two-peer **render-fidelity**; ≥4 simultaneous forges under budget all converge;
cross-chunk-wake pushes *both* chunks; **fragmentation** (delta > `NET_CHUNK_MAX` 28672
split/reassembled — host-push is a new cadence vs request/response); handshake refuses a v2
peer; bandwidth + diff-scan CPU probe.

### M6 — Release hardening & ship 0.4.0 (only when asked)
`make check` green; both targets warning-clean; **version-string sweep** (kill hardcoded
`"VOXEL ENGINE 0.3"` at `main.c:709`, fix `net.h:32` comment, grep for stray `0.3`/`2`);
**win-target determinism run** (`testsim`+`testdeterminism` under wine — closes the
gcc-vs-mingw gap the host-auth rationale assumes away); **fixture-based 0.3-save-into-0.4**
round-trip; **worst-case frame-budget probe** (new `VOXEL_*` N-forge hook → 30 FPS holds);
release binary has no `sim_state_hash`. Then the ritual: drop `-dev`, CHANGELOG (state the
2→3 net break + "0.3 saves load unchanged" + "WG_GEN_VERSION held at 2"), `make archive`, tag,
push.

**Dependency note:** M3 depends on M1/M2 by *sequencing* (land the soul early), not technically
— if reachability ever forces the forge after M4, the refactor isn't artificially blocked.

## 5. Deferred to 0.5 (correctly out of 0.4)
Deterministic lockstep CA (+ its remaining prerequisite gap: full-resolution payload —
**edit tick-stamping is now done in 0.4**); full-resolution `heat[]`/
`latent[]` in the wire/persist payload; cross-chunk **fluid** flow
(transported mass across async-evicting faces = a conservation hazard; heat is a field, fluid
isn't); **combustion** (a player-lit fuel CA — the 0.5 "fuel-as-progression" headline);
**warmth-as-survival**; radial fluid flow / the full water cycle / wind / void+atmosphere;
the fine-voxel grid shrink; crafting/tools beyond casting; client-side discovery derivation
(a `MSG_DISCOVERY` side-channel); persisted `latent[]`; ore baked into worldgen (bumps
`WG_GEN_VERSION`).

## 6. Save / network compatibility
- **`WG_GEN_VERSION` stays 2** — the headline is *runtime* behaviour, not changed terrain;
  the forge/ore are a post-gen pass, so 0.3 saves' unmodified chunks regenerate byte-identical.
- **`PERSIST_FORMAT_VERSION` stays 1** — payload stays `mat|temp|fill`; CA mutates exactly
  those bits. The one change: CA-mutated chunks set `CHUNK_MODIFIED` (a flag, not a layout
  change). `latent[]`/`heat[]` sub-quantum bank stay transient (lossy re-seed, measured by the
  M4 test). `world_tick` not persisted.
- **`NET_PROTOCOL_VERSION` 2→3** — a 0.3 client and 0.4 host cleanly refuse each other. **Both
  must update to play co-op.**
- A **fixture-based 0.3-save-into-0.4 round-trip test** proves the compat claim end-to-end.

## 7. Verification (all fails-on-old, all under `make check`)
1. Single-chunk readonly-invariance (the regression anchor, green every milestone).
2. Cross-chunk-seam unit tests: heat-across-a-face + conservation; **heat** order-independence;
   **melt-on-seam** order-independence (guards the PHASE-1.5 split).
3. Save/evict/reload determinism (surfaces the `heat[]`/`latent[]` re-seed loss in SP).
4. Two-peer render-fidelity via the GL-free driver (+ multi-forge, cross-chunk-wake,
   fragmentation, client-ticks==0).
5. M6: win-target determinism under wine; fixture compat; worst-case frame-budget probe.

## 8. Decisions (RESOLVED 2026-06-24)
1. **Forge placement → SURFACE chunk** (sphere top, cy≈8). HOME is buried in the core; the
   forge goes where the player spawns. *(default confirmed)*
2. **Heat source → held LAVA POCKET** (zero new sim code, keeps M1 lean). Player-lit
   **combustion** is the 0.5 "fuel-as-progression" headline. *(0.4 heat is a natural source the
   player traps, not yet player-made.)*
3. **0.4 scope endpoint → SHIP M5** — multiplayer sees the living world (host-streaming).
4. **Journal → HOST-AUTHORITATIVE** for co-op (per-client discovery side-channel is 0.5).
   *(default confirmed)*
5. **Warmth-as-survival → SLIPPED to 0.5.** 0.4 = heat-as-progression only.
6. **Lockstep prep → tick-stamped edits PULLED INTO 0.4** (M3a). Closes 0.5-lockstep gap (2)
   early; 0.5 lockstep then needs only the full-resolution payload (gap 1).
