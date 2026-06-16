# Changelog

All notable changes to the Voxel Engine. Versioning is Factorio-style
`MAJOR.MINOR.PATCH` (see `src/version.h`): pre-1.0 while the game takes shape;
`PATCH` = save-compatible fixes, `MINOR` = new gameplay/features, `MAJOR` = 1.0
or a breaking overhaul.

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
