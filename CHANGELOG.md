# Changelog

All notable changes to the Voxel Engine. Versioning is Factorio-style
`MAJOR.MINOR.PATCH` (see `src/version.h`): pre-1.0 while the game takes shape;
`PATCH` = save-compatible fixes, `MINOR` = new gameplay/features, `MAJOR` = 1.0
or a breaking overhaul.

## 0.5.0 — unreleased — Finite Flowing Water (in development)

0.5 ships the soul of the project: **finite water that flows downhill under radial
gravity, pools in basins, and settles** — rivers from a spring or a placed source, on a
planet finally grown to a real size at a finer **0.5 m voxel grain**. The planet starts
**dry**; every drop comes from a source and is conserved. The full water *cycle*
(evaporation / cloud / snow / ice / melt) is **deferred to 0.6**. Built milestone by milestone
behind the `make check` gate; see `PLAN-0.5.md`.

> Compatibility (planned): a **hard break** at the grain flip — `WG_GEN_VERSION` 2→**4**,
> `PERSIST_FORMAT_VERSION` 1→2, `NET_PROTOCOL_VERSION` 3→4. 0.4 saves are **refused, not
> migrated** (a 32 m pebble of 1 m voxels cannot meaningfully become a 256 m planet of 0.5 m
> voxels). `WG_GEN_VERSION` reached 4 because the terrain gained radial relief late in 0.5-dev;
> any earlier 0.5-dev save is likewise refused.

### Added
- _(M0)_ `src/units.h` — the **metres↔voxels split** (`VOX_GRAIN_MM`, `M2V`/`V2M`), the
  compile-time grain knob the fine-grain future rests on. Additive and applied nowhere yet
  (the grain flip is M2); its algebra is asserted by a new `test_grain` suite, and a
  `test_water` determinism harness slot is wired into `make check` (the binary-fill water CA
  lands at M3). This milestone changes no engine behaviour — the build is byte-identical to
  0.4.0 but for the version string.
- _(M1)_ **Sparse-air chunk storage.** `Chunk.voxels` becomes a pointer: a non-uniform chunk
  borrows a 16 KiB block from a new WorldStore slab sub-pool; a **uniform-air** chunk (the
  ~72%-empty majority of a resident window, measured) holds `voxels == NULL` + a `uniform_word`
  and borrows none. All cross-chunk neighbour reads route through a new `chunk_vox()`; the
  mesher/lighter/CA early-out on uniform chunks; `world_realize`/`world_set_uniform` +
  copy-on-write back a chunk on first edit or CA wake. The resident voxel working set drops
  from a dense **23.8 MiB to ~6.7 MiB** (431 realized blocks of a 768-slab pool) and the chunk
  -record pool from 27.8 MiB to ~153 KiB. Still on the 1 m grain / R=64 planet (the grain flip
  is M2), so **0.4 saves still load** (the persist format + `WG_GEN_VERSION` are unchanged).
  New `test_sparse` suite; ASan/UBSan-clean over streaming + the CA + multi-session teardown.
- _(M2)_ **The grain flip + a real planet (the hard break).** `VOX_GRAIN_MM=500` is now
  _applied_: every physical constant (player size/speed/gravity/`r_p`/substep, edit reach, fog
  distances, dirt depth) is re-expressed in **metres** via `M2V`/`V2M`, so the feel is preserved
  at the finer grain. The planet grows from a 64-voxel pebble to **R=512 voxels = a 256 m
  radius** (~1.6 km around). The resident vertical band now **follows the player** (the surface
  sits at high Y, so a fixed band no longer works) — a 9-layer window re-anchored on the
  player's chunk-Y each frame, keeping the resident count at 1521. Float32 radial gravity is
  retained with a regression assert (worst-case `up` error **0.00005 vox** at R=512, far under
  the 0.25 bound). **HARD BREAK:** `WG_GEN_VERSION` 2→3 and `PERSIST_FORMAT_VERSION` 1→2, so
  **0.4 saves are refused, not migrated** (a 32 m 1 m-voxel pebble cannot become a 256 m 0.5 m
  -voxel planet). No water yet — this milestone verifies the 256 m sphere is walkable. New
  `test_grain` float-precision sweep; both targets `-Wall -Wextra` clean; ASan-clean over a
  fly-across-the-pole + CA session.
- _(M3)_ **Finite flowing water — the binary-fill CA (the headline).** Replaces the
  partial-fill liquid model (which the prototypes proved can never settle) with **binary
  occupancy**: water is fill `{0,15}` and a whole voxel falls along the chunk's **radial
  "down"** (cached per-chunk toward the planet centre) and flows-to-descent laterally, so it
  **settles** (terraced) and **conserves** exactly. A **spring** (`MAT_WATER` + `sim_set_spring`)
  is the one inexhaustible source; the demo gains a pole-side water spring that pools in a
  carved pocket. Rides the 0.4 heat CA's active-front + READ/COMMIT seam (single-chunk;
  cross-chunk flow is M4). The partial-fill head field + connected-body finisher are kept
  compiled but **gated off** (M4 replaces the finisher with a radial shell-snap and re-enables
  it). New `test_water` (settle/conserve, radial-from-all-6-faces, spring, bounded dam-break,
  heat+water coexistence, determinism); the old partial-fill `test_sim` fluid cases are adapted
  to the binary invariants. Both targets clean; ASan-clean over a forge+water fly session.
- _(M4)_ **Water crosses chunk seams** — a river can now flow down across chunk boundaries,
  not stop at them. A boundary water voxel whose radial-down neighbour is out-of-chunk asks the
  WorldCA (`SimXFlowFn` on the SimState) whether the chunk below has air there; if so the move is
  enqueued and applied **atomically after all chunks commit** (materialise the neighbour cell +
  revert the source = conserved), realizing a uniform-air neighbour first. Deterministic
  (enqueue order, first-wins on corner conflicts), crash-safe (residency-checked), single-chunk
  sims keep closed walls (`fluid_xfn == NULL`). New `test_water` cross-chunk seam case (enqueue →
  cross → conserve). _The radial shell-snap finisher (flat lake surfaces) is **deferred to 0.6**:
  binary fine voxels settle natively and the river-focused foundation doesn't need it — see the
  design note. `world_pin` (keeping an active water chunk from eviction mid-flow) is also a 0.6
  follow-up; the deposit apply is already crash-safe without it._
- _(M5)_ **Multiplayer sees the living water — host-authoritative water streaming.** A client
  now watches the host's rivers flow without simulating anything itself: the WorldCA (heat +
  water) runs **only** on the host/single-player (clients run zero CA), and every CA-touched
  chunk is streamed to clients as a delta-from-seed. Because the wire already carries the **full
  voxel word**, the water `fill` field rides the existing channel for free. The delta is now
  **run-length encoded** (`NET_PROTOCOL_VERSION` 3→4): consecutive same-value voxels collapse to
  one `(start, len, voxel)` record, so a flooded 4096-voxel chunk — the dam-break worst case —
  drops from **~24 KB to 22 B** on the wire, which is what keeps a flowing river inside a home
  uplink's budget. `NET_CHUNK_MAX` is resized so the RLE worst case (a chunk where no two
  neighbouring voxels match, e.g. a fine heat gradient) still fits without truncation. New
  `test_water_net` drives a real two-peer loopback — host floods a chunk, serves it through the
  real RLE codec, pushes it over the socket, and the client's `chunksync_apply` reconstructs the
  water voxel-for-voxel; `test_chunksync` gains the flood-to-one-run and worst-case-no-truncation
  cases. (0.4↔0.5 peers were already refused at the handshake by the M2 `gen_version` bump.)
- _(M6)_ **Release hardening.** A new `test_persist` case asserts the headline compatibility
  promise directly: a save stamped at the **0.4 generator version (gen 2)** is **refused on load
  by a 0.5 build (gen 3) — it reads as a MISS and regenerates, never mis-interpreted** as 0.5
  data (a 32 m-pebble voxel cannot be re-read as a 256 m-planet voxel). Verified for ship: both
  targets `-Wall -Wextra` clean; the determinism symbol `sim_state_hash` is absent from the
  release binary (compiled in only under `-DVOXEL_DETERMINISM_HARNESS`); the version string has a
  single source of truth in `version.h`; the integrated heat-forge **and** water-spring sim runs
  bounded (5000-tick warmup in ~2 s, ~91 MB RSS, no runaway active set).
- _(M6)_ **Cross-platform determinism — verified, and a real bug fixed on the way.** The integer
  CA must compute the world byte-for-byte identically on the 32-bit Windows target as on the dev
  Linux box. New `det_hash_dump` fingerprints fixed heat/water/combined scenarios via
  `sim_state_hash`, and two `make` gates compare them: **`detcross`** (native LP64 vs `-m32` ILP32
  — the wine-free data-model check) and **`detwine`** (native vs the actual `i686-w64-mingw32` PE
  run under wine — the real shipped artifact). Both now report **identical hashes** across all
  three data points (Linux-64, Linux-32, Windows/wine). Standing this up exposed a latent **M1
  regression**: `sim_state_hash` hashed `sizeof(voxels)` bytes, which became the **pointer width**
  (8 on LP64, 4 on ILP32) once `Chunk.voxels` turned into a pointer — so it folded only the first
  1–2 voxels, leaving the **entire voxel array (including water `fill`) effectively unhashed** in
  every determinism test (they passed only because `heat[]` carried the signal). Fixed to hash the
  full `CHUNK_VOXELS`-word array (or the uniform word for a slab-less air chunk).

### Added (continued)
- **Water levels into flat pools instead of stair-step terraces.** Binary water settled by gravity
  + flow-to-descent alone never spreads to flatten a surface, so lakes/pools came out as ugly
  terraced mounds. The connected-body **surface-leveling finisher** is now revived for binary
  water: when a body settles, it is snapped to a flat (within-one-voxel) surface in **whole cells**
  (no fractional fills), conserving its volume exactly. It fires on a **settle** trigger (binary
  water settles to a fixed state, so the old limit-cycle detector never confirmed) and loaded/static
  bodies are woken on bind, so a **saved terraced lake flattens as it loads** (measured: a static
  4-step lake levels in 2 ticks; a 96-cell pile in 12; both conserve). Enabled on the **−Y polar
  cap** (where the world-Y column math is correct — water on the sphere's far sides stays terraced
  until a radial version in 0.6). An adversarial review caught + this fixes two real defects before
  ship: a conservation break where a column's vessel-extension could swallow a *separate* same-
  material body and drain it (now bodies are per-body-membership walls, verified 6→6 not 6→4), and a
  dead trigger (the claimed "rise clause" didn't exist, so static stalls never fired). Cross-platform
  deterministic (`LEVELED` hash identical on Linux-64/32/wine).
- **Placeable water springs — make your own rivers.** A new **`MAT_WATER_SOURCE`** material
  (hotbar slot 4, the bright-cyan swatch) is an inexhaustible spring: `sim_init` auto-registers
  every `MAT_SPRING` voxel via `sim_set_spring` (the material *is* the spring, so it persists
  across save/reload like emissive lava is a held heat source), and it emits plain `MAT_WATER`
  that flows + pools under gravity. Placing one wakes the chunk's sim (host/SP) and registers it
  immediately, so a source placed at the top of one of the new hillsides flows **downhill as a
  river** and pools into a lake at the bottom. _(On flat ground it just pools — a spring keeps
  flowing only while its outflow keeps draining downhill.)_ The demo forge's spring is now a
  `MAT_WATER_SOURCE` too. **Fixes** the demo spring that only ever dropped a single block: the
  forge's emissive lava had filled the 16-slot held-source table, so `sim_set_spring` failed
  silently — lava is now held by its flag and takes no slot, freeing the table for real springs.
- **Per-world generator pinning — worlds survive future worldgen changes** _(the save-
  compatibility contract starts here)_. A world is now tied to the generator version it was
  **created** with: its save records that version (every region header already stamps it), and on
  load the engine **peeks** that version (`persist_peek_gen_version`) and **regenerates the world
  with it** rather than with the build's current generator. The build **retains every supported
  generator** and dispatches by version (`worldgen_select_version` / `worldgen_version_supported`);
  the pre-relief smooth sphere is kept as **gen v3**, the relief planet is **gen v4**. So a world
  you keep loads as the world it was, even after a later version changes the default terrain — and
  a save is refused only if its generator is genuinely **unknown** (or the file is corrupt), never
  merely because it is *old*. To get new terrain you create a new world. Multiplayer stays correct:
  the host advertises the **world's** generator version (not the build's), so a peer that would
  regenerate different terrain is cleanly refused at the handshake instead of silently desyncing.
  _Contract going forward: changing terrain means ADDING a `gen_fill_vN` + a dispatch case + a
  `worldgen_version_supported` entry and bumping `WG_GEN_VERSION` — **never editing a retained
  generator** (an old generator must keep producing byte-identical output forever, or its worlds
  corrupt). New `test_worldgen` (v3≠v4 dispatch) + `test_persist` (peek → pin-load → wrong-version
  miss) cover it._
- **The planet has terrain — hills, ridges and basins** _(`WG_GEN_VERSION` 3→4)_. The smooth
  dry sphere now displaces its surface radius per direction by a deterministic **integer 3D
  value-noise** field, so water placed on it finds a downhill path and pools in hollows instead
  of sitting on a featureless ball. The displacement is sampled on the **surface point** (the
  unit direction × R, found with an integer `isqrt`) so it's a clean per-direction height
  (hills, not 3D caves), evaluated only in the thin transition shell near the surface (the deep
  core and far space short-circuit, so the noise is paid only where it matters). It is **flattened
  to zero near the spawn pole** so the forge, the spawn, and the pole chimney stay on predictable
  solid crust. Pure integer end to end — the new `det_hash_dump` **WORLDGEN** fingerprint hashes
  byte-identically across Linux-64, Linux-32 and the Windows build under wine. `worldgen_chunk_
  all_air` is now conservative against a relief peak (`R+AMP`) so sparse-air never drops a ridge.
  New `test_worldgen` asserts the relief is bounded, deterministic, pole-flat, non-trivial, and
  continuous; amplitude/period are tunable `#define`s in `worldgen.h` (currently ±10 m over
  ~32 m features). _The natural water **cycle** that fills these basins (snow/melt → rivers →
  sea) is still 0.6; for now water comes from a placed source and flows over the new terrain._

### Fixed
A pre-ship adversarial bug hunt across the whole 0.5 delta (8 lenses, every finding
verified by independent skeptics) surfaced — and these fix — the following, each with a
regression test where one was missing:
- **Cross-chunk water flowed into the WRONG chunk** _(critical, M4)_. The fluid cross-seam
  path passed a `SIM_NEIGH` face index (`{+X,-X,+Y,-Y,…}`) straight to `neigh[]`, which is in
  the **swapped** Face-enum order (`{-X,+X,-Y,+Y,…}`) — so radial-down (−Y) fetched the chunk
  **above**. The heat path already converts with `^1`; the fluid path now does too
  (`sim.c`). The M4 seam test was passing only because it wired neighbours backwards — it now
  uses the production convention and **fails if the `^1` is dropped**.
- **Water pooled on a chunk's down-face boundary never settled** _(high, M4)_. The "keep a
  boundary voxel awake to retry the cross-move" clause didn't check whether the neighbour
  below was air, so water resting over solid crust spun forever — and, since active chunks are
  never retired until eviction, eventually filled the 48-sim pool and **stalled all cross-chunk
  flow**. The clause now stays awake only when the cell across the seam is actually air.
- **Multiplayer clients accumulated phantom water** _(high, M5)_. `chunksync_apply` patched the
  delta-from-seed onto the resident chunk **additively**, so a cell the host reverted to
  seed-equal (omitted from the delta — e.g. water that flowed onward) lingered on the client
  forever. Apply now resets the chunk to seed before applying the delta (new
  `world_reset_to_seed`); a new `test_water_net` drain case guards it.
- **Cross-seam water was reset to 20 °C and left a stray-fill air cell** _(low, M4)_. The
  deferred deposit now mirrors the in-chunk move: the destination keeps its own temperature and
  the drained source becomes fill-0 air (was minting fresh 20 °C water / `fill=15` air).
- **Backpressured clients could miss a chunk's final state** _(M5)_. The host cleared a chunk's
  stream flag even when a slow client was skipped; if the chunk then settled it was never
  re-sent. `net_host_push_chunk` now reports skips and the flag is kept until every client
  receives it.
- **Forge chimney centre was capped** _(M2 grain)_ and a defensive guard added against a
  (theoretical) evicted-chunk reuse in the sim retire loop. Stale R=64-era comments corrected.

## 0.4.0 — 2026-06-24 — The World Comes Alive

0.4 unfreezes the cellular automaton: heat diffusion + melting/freezing tick
**world-wide across every loaded chunk, with heat crossing chunk faces**, lighting
the forge/metallurgy discovery loop and surfacing it in an in-world **journal HUD**.
Multiplayer sees the living world via **host-authoritative streaming**. Stays on the
1m uniform grid (the fine-voxel shrink is a deferred 0.5 thing). Built milestone by
milestone behind a `make check` regression gate; see `PLAN-0.4.md`.

> Compatibility: 0.3 single-player **saves load unchanged** (`WG_GEN_VERSION`
> held at 2, on-disk format unchanged); 0.3 and 0.4 **network peers are incompatible**
> (`NET_PROTOCOL_VERSION` 2→3) — both must update to play co-op.

### Added
- **World save/load + a Minecraft-style menu.** "Single Player" / "Host Game" now open a
  **world list** (select / create / delete) instead of a single fixed world. Each world is a
  named save directory (`saves/<slug>/` with a `world.meta` recording its display name + seed);
  **Create New World** takes a name + an optional seed (blank → random, typed → numeric or
  string-hashed). The list scrolls past 7 worlds, sorts by name, and confirms deletes. Hosting
  hosts the selected world. Existing single-player saves still load via the env/`VOXEL_SAVE`
  path. New cross-platform world enumeration/create/delete in `persist.c` (opendir/readdir on
  POSIX, FindFirstFile on Win32). _(Win32 fs path is compile-verified; not yet run under Wine.)_
- **Placement hotbar HUD.** A bottom-centre row of five colour swatches (Stone · Dirt · Copper ·
  Water · Lava) drawn from each material's own colour, numbered to match the `1`–`5` select keys,
  with the active slot ringed white and its material name labelled above — so it's always visible
  which block a place action will drop.
- _(M0)_ `make check` aggregate regression gate (runs every unit suite, fails on the
  first non-zero exit) + a GL-free **CA determinism harness** (`testdeterminism`,
  `sim_state_hash`) — the substrate the multiplayer determinism work is verified on.
- _(M1)_ A **reachable surface forge**: the heat sim's HOME chunk moves from the buried
  core to the crust directly under the spawn pole, where `demo_decorate` carves a stone
  **cavity** holding a lava pool with a copper charge submerged — the trapped heat melts
  the charge past 1085 °C (the emergent smelter), now visible from the surface.
- _(M2)_ The engine's first **in-world progression journal**: a discovery **toast** when a
  new transition is first observed, and a toggleable **journal panel** (`J`) listing recent
  discoveries + the current capability tier — surfacing `progress.c` (console-only since 0.1)
  via the 0.3 text/UI primitives. The HUD reads the observer through a `const` handle, so the
  read-only "remove it and the sim is byte-identical" invariant is compiler-enforced.
- _(M3a)_ A global **logical clock** (WorldClock): the heat sim's tick is now owned by the
  frame loop, not self-counted — the shared clock M3b feeds to every chunk, and the clock
  player edits are stamped against (the lockstep hook). Byte-identical to 0.3 for one chunk.
- _(M3b)_ The **world-wide CA container**: the heat sim is no longer one bound chunk — a
  capped active set (forge + future woken neighbours) ticks in canonical `(cy,cz,cx)` order on
  the shared clock, with a budgeted sim-remesh and a `CHUNK_MODIFIED_BY_SIM` flag. Faces are
  still closed, so it is byte-identical to 0.3 for the forge (ASan/UBSan-clean over sessions).
- _(M4)_ **THE HEADLINE — heat crosses chunk faces.** The per-chunk tick splits into a READ
  pass and a COMMIT pass; the WorldCA runs every active chunk's READ before any COMMIT, so a
  cross-chunk boundary read sees the neighbour's **start-of-tick** state and a seam diffuses
  exactly like an interior face (order-independent — proven by new GL-free seam tests). Heat
  reaching a chunk boundary **wakes the neighbour chunk** (deferred, enqueue-only), so the
  CA propagates world-wide. The forge gains a lava chimney whose heat visibly crosses into the
  crust above. Single-machine deterministic; ASan/UBSan-clean.
- _(M5)_ **Multiplayer sees the living world** — host-authoritative CA streaming. The host
  runs the CA and **pushes** each changed chunk's delta to clients (`net_host_push_chunk`,
  backpressure-safe); **clients run no CA** and render the host's stream. One authority means
  no cross-peer divergence. `NET_PROTOCOL_VERSION` 2→3, so 0.3 and 0.4 peers refuse each other
  at the handshake. Verified by a GL-free push test + a two-process loopback (client joins and
  renders the host's melting forge).
- _(M6)_ Release hardening: the connect-screen title derives from `version.h` (no hardcoded
  "0.3"); a **0.3.0 save loads in 0.4** (verified via the archived 0.3.0 binary round-trip);
  the determinism hash is absent from release binaries; both targets stay `-Wall -Wextra`
  clean with the full `make check` green. _(Win-target runtime-determinism + a Pentium-M
  frame-budget probe await on-target testing — Wine was unavailable on the dev box; the sim
  hot loop is integer-only, so cross-toolchain bit-exactness is high-confidence.)_

## 0.3.0 — 2026-06-17 — Multiplayer

Play the planet together. 0.3 turns the single-player sandbox into host-authoritative
**co-op for up to 4 players**, and — to make hosting/joining usable in-game — adds the
engine's first **text rendering + menus**. Save-compatible with 0.2.x single-player
worlds (the generator is unchanged); multiplayer requires every peer on the same build
(the join handshake refuses a game/generator-version mismatch).

The world is a deterministic function of its seed, so joining transfers **nothing but
the seed** — each machine regenerates the identical planet locally, and only player
poses and block edits cross the wire. Every feature shipped with an adversarial review
pass + headless tests; 8 socket/lifecycle defects were found and fixed before release.

### Added
- **Host-authoritative multiplayer (up to 4).** One player hosts and plays; others join.
  A version-gated handshake shares the seed (no map download); player **avatars** are
  relayed and interpolated to hide internet jitter; **block edits** are relayed live;
  and **on-demand chunk-delta sync** sends a chunk's voxels-that-differ-from-seed when a
  client streams it in, so a joiner sees the host's *existing* build, not pristine
  terrain. Single-threaded, non-blocking **TCP**; XP-safe (Winsock2) and Linux (BSD
  sockets) behind a tiny `net_*` layer. Movement is client-authoritative (trusted
  friends; no anti-cheat). *(net.c, net_linux.c, net_win32.c, chunksync.c)*
- **In-game connect screen** — Single Player / Host / **Join (type a server IP)** / Quit
  — and an in-game **pause menu** (Resume / Fullscreen / **Main Menu** / Quit). ESC opens
  the pause menu (the window-X still quits) and frees the cursor; **Main Menu** leaves a
  session and returns to the connect screen so you can host/join/switch without
  relaunching. *(main.c)*
- **The engine's first text rendering**: an embedded 8×8 bitmap font + `render_text` /
  `render_ui_rect` (screen-space, via the overlay shader — no new shader/texture), plus
  platform **text input** (`plat_text_poll`). The reusable basis for future HUD/UI.
  *(render.c, platform_linux.c, platform_win32.c)*
- **In-engine fullscreen toggle** (X11 EWMH / Win32 borderless) + arrow-key/Enter menu
  navigation. *(platform_*.c)*
- **Reachability docs + launch wrappers**: `MULTIPLAYER.md` (same-LAN IP / mesh tunnel /
  port-forward) and `scripts/{host,join}.{sh,bat}` — though hosting/joining is now
  in-game. All `VOXEL_*` env vars + the scripts remain for headless/CI.

### Changed
- `main()` is restructured into a **session loop** (connect screen → set up a session →
  play → Main Menu → tear down → repeat), so a session's world/sim/persist/net are built
  and freed cleanly each time (verified leak/double-free-free with AddressSanitizer over
  repeated cycles). Headless/scripted runs (`VOXEL_SHOT`, `VOXEL_HOST`/`VOXEL_CONNECT`)
  bypass the menu and run exactly one session, preserving the 0.2 behaviour.

## 0.2.1 — 2026-06-16 — Bug-fix release

Save-compatible with 0.2.0 (no gen/format change). Four defects fixed: two
reported from play, two surfaced by a pre-release adversarial bug hunt (parallel
per-subsystem static review, each finding refuted by two independent lenses, then
verified against the running code).

### Fixed
- **Builds now persist.** Persistence was silently disabled at runtime: the save
  dir is two levels deep (`saves/<seed>`) but `persist_mkdir` did a single `mkdir`
  of the leaf, which fails with `ENOENT` when the parent `saves/` does not exist
  yet — so `persist_open` returned NULL, the store stayed off, and **every edit
  was ephemeral** (regenerated from seed the moment a chunk evicted and reloaded,
  i.e. as soon as you roamed away and back). `persist_mkdir` now creates every
  missing parent (`mkdir -p`). Added a regression test (open at a path whose
  parent is absent). *(persist.c, test_persist.c)*
- **Player no longer tunnels through the planet.** The sphere collider's
  deep-penetration fallback (when the body's center is fully buried in a voxel and
  the contact normal is lost) pushed along world **+Y** — a leftover flat-world
  assumption. On the far hemisphere the radial up points toward −Y, so the push
  drove the body **deeper**, the into-surface velocity was never cancelled, and a
  fast or off-axis fall punched straight through the solid planet and out the far
  side (`on_ground` never latching). The fallback now pushes along the **radial
  up**, so it stops at the surface everywhere. *(player.c)*
- **Camera no longer flickers at the poles.** The tangent-frame basis was rebuilt
  each frame from a reference axis with a hard switch at `|up.y| = 0.99`; flying
  near a pole, tiny jitter across that threshold snapped the whole frame ~180°
  (the view "reversing itself"). The heading is now a persistent vector,
  **parallel-transported** onto each frame's tangent plane and rotated by mouse
  yaw about the radial up — no global reference, so no pole singularity. (Heading
  drifts slightly over a full lap — sphere holonomy — which is accepted.)
  *(main.c)*
- **Headless captures use the live framebuffer size.** `VOXEL_SHOT` wrote the
  fixed 1024×768 creation size; under a window manager that resizes the window on
  map (tiling / forced-maximize) that captured only a corner crop. It now reads
  the live drawable size, matching the projection's aspect path. *(main.c)*

## 0.2.0 — 2026-06-16 — Spherical planet

The world becomes a **small voxel planet**. The cycle opened aiming at water, but
the headline that emerged is a genuine ball of cubes with a **center of mass**:
gravity points toward the core *everywhere*, you walk all the way **around** the
surface, and the camera's "up" rotates to follow the curvature — the horizon curves
because the ground does, not because of a shader. Not save-compatible with 0.1.x
(the world is now a sphere, not a heightmap; `WG_GEN_VERSION` 1 → 2).

### Added
- **Spherical voxel asteroid + radial gravity** *(the 0.2 headline)*. Worldgen fills
  a solid ball with a pure-integer squared-distance test — stone core, a 3-voxel dirt
  crust, air outside; radius 64 about a fixed center of mass — so it is deterministic
  and seed-independent (one fixed asteroid). Player physics replaces the −Y AABB with
  a rotation-invariant **sphere collider** under radial gravity (`down = normalize(center
  − pos)`), so you stand, walk, and jump correctly at *every* point on the planet —
  pole, equator, or the far side. The camera builds a per-position **tangent frame**
  (radial up + a pole guard) so the view stays upright and the horizon curves as you
  move. The greedy mesher needed **no change** (it meshes only the spherical shell);
  the streaming window grew to a tall band that brackets the ball.
  *(worldgen.c, worldgen.h, player.c, player.h, main.c, world.h)*
- **Block break / place.** A voxel raycast from the eye to the first solid block:
  break the targeted block or place the selected material against it, with a wireframe
  highlight on the target and a guard against placing inside yourself. Edits flow
  through the world (remesh + persist). *(main.c, world.c)*
- **Crosshair + target highlight** overlay (screen-space). *(render.c, main.c)*
- **Player embodiment.** A real body with gravity, collision, walking, and jumping —
  now in the radial / sphere form above (you are no longer a free-flying camera).
  *(player.c, main.c)*

### Changed / prototyped
- **Fluid sim — explored, then frozen.** This cycle prototyped fill-height water
  surfaces, lake leveling, and true **communicating vessels** (a conserving connected-
  body finisher: two basins joined by a channel settle to one level). It is sound on
  the flat −Y world and kept in git history, but is **out of scope on the sphere** and
  intentionally inert there, pending a radial fluid rewrite (water pooling toward the
  core) in a later release. The flat-world lava/water demo decoration is likewise
  superseded by the asteroid. *(sim.c)*
- **Flat ambient lighting on the planet.** Skylight still bakes top-down (+Y), which
  would leave a sphere lit only on its cap, so the opaque shader's ambient floor was
  raised — AO still gives shape. True radial skylight is deferred. *(render.c)*

### Fixed
- **Horizontal mouse-look was inverted** on the new radial camera (wrong handedness on
  the yaw term). *(main.c)*
- **Block edits now actually re-render.** `chunk_set` raises the dirty-mesh flag that
  `remesh_enqueue` also uses as its "already queued" marker, so an edited chunk was
  flagged dirty but **never queued** — the voxel changed (collision and the raycast saw
  it) while the on-screen mesh stayed stale (break a block and it lingered; place one
  and it was invisible). The edit path now clears the flag before enqueuing.
  *(world.c)*
- **Place-guard reoriented.** It used a world-Y box from the player's feet, which on
  the planet's side blocked the ground in front of you; it is now an orientation-free
  sphere-overlap test. *(main.c)*

## 0.1.1 — 2026-06-15 — Bug-fix release

Save-compatible. The first dedicated bug hunt across the foundation (parallel
static review of every subsystem + an AddressSanitizer/UBSan + headless-stress
pass, each finding adversarially verified). Five confirmed defects fixed; three
latent/cosmetic ones with no shipped trigger are tracked for later.

### Fixed
- **Win32 mouse capture is now released on focus loss** *(high — the XP ship
  target)*. Alt-Tabbing out of a live session used to leave capture engaged, so
  the per-frame recenter kept yanking the **system cursor** back into the
  backgrounded window 30–60×/sec, making the desktop unusable until the engine
  was killed. The window now drops the grab on `WM_ACTIVATE`/deactivate and
  re-arms it on refocus, the per-frame recenter is gated on actually holding the
  foreground, and the camera no longer snaps on return. *(platform_win32.c)*
- **Chunk generation no longer drops chunks under fast movement.** The gen queue
  is a fixed ring; sustained motion (≳ one chunk/frame — sprint/teleport, or the
  headless `VOXEL_FLY` stress mode) could lap its head and silently drop queued
  chunks, collapsing the streamed window and leaving **permanent holes** once the
  player stopped. The queue is now rebuilt deduped each move (bounded by the
  window size, so it can never lap) with a defensive ring-full guard on every
  enqueue. *(world.c)*
- **Live window resize / maximize now works.** The GL viewport was never resized
  (frozen at the 1024×768 creation size) and the projection aspect ratio was
  hardcoded, so resizing clipped and distorted the view. Both backends now handle
  the resize event (`WM_SIZE` / `ConfigureNotify`), call `glViewport`, and expose
  the live size (`plat_get_size`); the camera recomputes its aspect per frame.
  *(platform_win32.c, platform_linux.c, platform.h, main.c)*
- **A held liquid source can no longer drain to an invisible "phantom".** A
  non-spring held liquid source (incl. `MAT_EMISSIVE` lava) donated its fill away,
  hit fill 0, then never reverted to air, never slept, and still rendered as a
  glowing cube (and could flood). Held heat-sources now conserve their fill in
  place (only an explicit *spring* flows + refills), and the mesher treats a
  drained liquid as empty. *(sim.c, mesher.c)*
- **Undefined behaviour in the heat codec removed.** `temp_to_heat` left-shifted
  a negative value for sub-zero temperatures (code 0 = −40 °C), which aborts every
  `-fsanitize=undefined` build. Replaced with a well-defined multiply (byte-for-
  byte identical on the shipped target); the sim suites are now UBSan-clean.
  *(sim.h, sim.c)*
- **Submerged surfaces are now visible.** The greedy mesher emitted a solid face
  only against air, so any solid touching a liquid (a lake bed, the floor/walls
  under water, a basin's interior) produced no geometry — diving in showed nothing
  but blue, and entering a pool made the surrounding stone vanish entirely (its
  only faces were back-facing outer ones). Solid faces are now emitted against a
  *see-through* neighbour — air **or** a translucent liquid — so submerged surfaces
  draw correctly from above and from inside the water. Bounded cost: only the
  solid↔liquid interface is added (a real visible surface); buried solid↔solid
  faces are still culled, so no interior bloat. The liquid pass is unchanged (no
  double face at the interface). *(mesher.c)*
- **Demo water no longer "floats."** The demo water block sat on an open pedestal,
  so it ran off the edges and pooled at the demo chunk's bottom face — which it
  can't fall through (cross-chunk vertical flow is deferred), leaving it hovering
  a chunk above the terrain instead of touching ground. It now falls into a walled
  stone **basin** (like the lava tub) and settles into a clean, contained pool on
  the pedestal floor. Pure demo geometry — the fluid sim itself is conservative
  (verified: zero fill lost over thousands of ticks for any contained body).
  *(main.c)*

### Known limitations / deferred
- **The simulation runs on exactly one chunk** (the HOME demo chunk). The rest of
  the streamed world is generated/lit/meshed/rendered but not ticked, and fluids/
  heat treat a chunk face as a closed wall — flow that reaches a boundary stops
  (which is why the demo water needs its basin). Activating the world-wide CA +
  cross-chunk flow is a **0.2-class** feature, tracked in `ARCHITECTURE.md`
  Appendix B (§3 Cellular Automaton). Not a 0.1.1 bug fix.
- **Underwater presentation is incomplete.** Submerged solid surfaces now draw,
  but the submerged-camera **blue tint/fog** and the **double-sided water surface**
  (seeing the surface plane from below) are deferred to **0.2** — tracked in
  `ARCHITECTURE.md` Appendix B (§5 Rendering Pipeline).

#### No shipped trigger
- Freezing with a placeholder `freezes_to`/`melts_to` of `MAT_AIR` would leave a
  stale-fill phantom — can't occur in 0.1.x (no sub-0 °C sink); waits for real
  `MAT_ICE`/`MAT_STEAM` with cold content.
- X11 first live frame can apply a one-frame camera jerk from pre-capture pointer
  motion (cosmetic, self-correcting).
- The demo's emergent smelter plateaus ~13 °C below copper's melt point because
  the column's air-exposed top vents heat — demo geometry, not an engine fault
  (the fully-enclosed sim test still melts). Will re-seat the demo lava later.

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
