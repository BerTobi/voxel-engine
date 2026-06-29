/* worldgen.h - Deterministic, seed-based procedural terrain.
 *
 * Binding source: ARCHITECTURE.md Section 7 / Section 8 - "The world generator
 * is a pure deterministic function gen(seed, cx, cy, cz) -> Chunk. Same seed,
 * same coordinate, byte-identical chunk, every time, on any machine." That
 * determinism is the load-bearing wall under streaming: a chunk evicted at the
 * window's trailing edge and regenerated when the player walks back must be
 * indistinguishable from the original, with ZERO disk I/O (Section 7's
 * "unmodified chunks have zero persistent storage cost"). Disk persistence of
 * MODIFIED chunks is Section 8 and deferred; this file is the regenerate-from-
 * seed engine the whole streaming milestone leans on.
 *
 * WHAT IT REPLACES: chunk_gen_flat (chunk.h/chunk.c), the dead-flat
 * single-height worldgen the static 4x1x4 grid used. A flat world makes
 * streaming INVISIBLE (every chunk identical), so this milestone introduces a
 * ROLLING-HILLS heightmap: a cheap deterministic value-noise surface of
 * (worldX, worldZ) so terrain VARIES and the moving window is plainly visible
 * as the player flies. chunk_gen_flat stays available (chunk.h) for the mesher
 * unit tests, which assert exact quad counts against a known-flat fill;
 * worldgen does not remove it.
 *
 * COST DISCIPLINE (Pentium M, single core): pure INTEGER value noise, no
 * float in the per-column hot path, no libc pow/sin. The height for a column is
 * a few hashes + integer bilinear interpolation across a coarse lattice, summed
 * over WG_OCTAVES (<=3) octaves. A 16x16 chunk = 256 columns; each column's
 * height is computed ONCE and the chunk filled by a vertical compare (the same
 * shape as chunk_gen_flat's world_y >= ground test, just with a per-column
 * ground). No divides on the hot path (lattice period is a power of two -> the
 * interpolation fractions are masks/shifts).
 *
 * DETERMINISM CONTRACT (binding, tested in test_world.c):
 *   - worldgen_height(seed, wx, wz) depends ONLY on its arguments. No globals,
 *     no statics, no time, no RNG state. Same inputs -> same output, every call,
 *     every machine, every build (integer-only, so no FP-rounding drift across
 *     the Linux dev host and the XP MinGW target).
 *   - worldgen_fill_chunk(c, cx, cy, cz, seed) is a pure function of its inputs
 *     into c->voxels (it ALSO sets c->cx/cy/cz and the gen flags); calling it
 *     twice with the same (cx,cy,cz,seed) yields byte-identical voxels[].
 *   - GEN VERSION: the world save header stamps a gen_version (Section 8). If
 *     the noise here ever changes, previously-unmodified chunks would regenerate
 *     differently. WG_GEN_VERSION below is that stamp; bump it on any change to
 *     worldgen_height / worldgen_fill_chunk output. Persistence (deferred) reads
 *     it; this milestone just defines and exposes it.
 */
#ifndef WORLDGEN_H
#define WORLDGEN_H

#include <stdint.h>
#include "chunk.h"

/* Bump on ANY change to generated output (Section 8 versioning). A save written
 * by an older version is "from an older generator" and must not silently
 * regenerate differently (the doc's default: refuse to load on mismatch). */
#define WG_GEN_VERSION   6u   /* 6: 0.5 DRAINAGE terrain + incised river VALLEYS (steep channels
                               * the water CA can flow down); 5 was smooth drainage; 4 small
                               * relief; 3 the smooth R=512 ball */

/* ---- Surface band (binding; the WorldStore vertical band brackets this) ---- *
 * The heightmap surface lives entirely within [WG_HEIGHT_MIN, WG_HEIGHT_MAX]
 * world-Y. world.h's resident vertical band [WORLD_BAND_Y0..Y1] (cy 0..1 ->
 * world-Y 0..31) is chosen to fully contain this range, so every surface voxel
 * is in a resident chunk. Keep WG_HEIGHT_MAX < (WORLD_BAND_Y1+1)*CHUNK_DIM so a
 * hill never pokes above the loaded band. With CHUNK_DIM=16 and band cy 0..1
 * the band covers world-Y 0..31; the surface sits comfortably mid-band. */
#define WG_SEA_LEVEL     14    /* baseline surface world-Y (rolling about here) */
#define WG_HEIGHT_AMP    8     /* peak-to-baseline amplitude (hills +/- this/2) */
#define WG_HEIGHT_MIN    (WG_SEA_LEVEL - WG_HEIGHT_AMP / 2)  /* 10              */
#define WG_HEIGHT_MAX    (WG_SEA_LEVEL + WG_HEIGHT_AMP / 2)  /* 18 (< 32)       */

/* ---- Strata (binding; the same dirt-cap-over-stone shape as chunk_gen_flat) - *
 * At/above the per-column surface -> air. The top WG_DIRT_DEPTH solid rows ->
 * dirt; everything below -> stone. (A grass cap / ore veins are future worldgen
 * passes; this milestone is stone+dirt+rolling-surface, enough to make movement
 * read.) Every solid voxel is stamped fill=15 and ambient temperature, exactly
 * the chunk_gen_flat convention, so light/mesh/sim see a consistent world. */
#define WG_DIRT_DEPTH    6     /* 0.5 M2: 6 vox x 0.5 m = 3 m crust (was 3 vox=3 m at 1 m) */

/* ---- Spherical asteroid (0.3 radial-gravity prototype) -------------------- *
 * The world is a single solid voxel BALL: a voxel is STONE where its world-space
 * distance from the center WG_PLANET_C* is <= WG_PLANET_R, with a WG_DIRT_DEPTH-
 * thick DIRT crust just beneath the surface, and AIR outside. Player gravity
 * (player.c) and the camera (main.c) point at this SAME center, so "down" is
 * radial everywhere. This replaces the rolling heightmap for the prototype; the
 * fill is a pure-integer squared-distance test, so determinism is preserved (and
 * it is independent of `seed` - there is one fixed asteroid). Center is at the HOME
 * column X/Z (=8) and lifted to CY so the ball spans world-Y [CY-R .. CY+R] inside
 * the resident vertical band (world.h WORLD_BAND_Y1 must reach (CY+R)/16). Bigger R
 * = gentler curvature; the band height + pool slack scale with R (see world.h). */
/* 0.5 M2: at the 0.5 m grain a voxel is 0.5 m, so R=512 voxels = a 256 m physical
 * radius (a ~1.6 km walk around) - a real planet, not the 32 m pebble R=64 would be.
 * Center lifted to CY=R so the ball spans world-Y [0 .. 2R]; the resident band now
 * FOLLOWS the player (world.h), so the surface being at high Y no longer needs a
 * tall fixed band. Kept near the world origin so float32 radial math stays precise
 * (test_grain asserts the worst-case `up` error < 0.25 vox at this R). */
#define WG_PLANET_CX     8     /* planet center, world voxels (X)                */
#define WG_PLANET_CY     512   /* planet center, world voxels (Y) = R (ball Y 0..2R) */
#define WG_PLANET_CZ     8     /* planet center, world voxels (Z)                */
#define WG_PLANET_R      512   /* planet radius, voxels (x0.5 m = 256 m physical) */

/* ---- Radial terrain RELIEF (0.5: hills, ridges, basins for water) ----------- *
 * The smooth sphere surface is displaced in/out by a deterministic INTEGER 3D
 * value-noise field sampled PER DIRECTION (the surface point = the unit direction
 * x R, found with an integer isqrt - so the displacement is a clean per-direction
 * height, giving hills + basins rather than 3D caves). Pure integer end to end, so
 * the planet stays byte-identical across the Linux/Windows targets like the rest of
 * worldgen. Amplitude is bounded by WG_RELIEF_AMP (fits well inside the resident
 * vertical band). The relief is FLATTENED to zero within sqrt(WG_POLE_FLAT_R2) of
 * the Y axis so the +Y spawn pole, the forge, and the pole chimney sit on flat,
 * predictable solid crust. Terrain is seed-INDEPENDENT (one fixed asteroid), so the
 * noise uses a fixed internal seed. */
#define WG_RELIEF_AMP        20    /* max in/out surface displacement, voxels (x0.5m = 10 m) */
#define WG_RELIEF_PERIOD_LOG2 6u   /* base feature lattice = 2^6 = 64 vox (32 m)             */
#define WG_RELIEF_OCTAVES    2u    /* period/amplitude-halving octaves (64 + 32 vox)         */
#define WG_RELIEF_SEED       0x5C0FFEE5u /* fixed: one asteroid, terrain seed-independent     */
#define WG_POLE_FLAT_R2      2304   /* flatten relief within sqrt(2304)=48 vox (24 m) of the Y axis */

/* ---- DRAINAGE terrain (0.5 gen v5: continents, ocean basins, a sea level) ---- *
 * v4's relief is small rolling hills (+/-20 vox over a 32 m lattice) - good texture,
 * but no LARGE-scale structure, so water just pools in the nearest local dip rather
 * than draining a long way. v5 keeps the same pure-integer 3-D value-noise machinery
 * but at a CONTINENT scale: a 256 m base lattice (period 2^8) with bigger amplitude,
 * so the surface radius swings widely - broad HIGHLANDS (surface well above R) and
 * broad ocean BASINS (surface well below R) with long connecting slopes. That gives
 * water a real downhill path AND somewhere low to collect. A SEA LEVEL radius
 * (WG_SEA_R) sits a little below the mean R, so the spawn pole (relief-flattened to
 * 0 -> surface == R) is dry LAND above the waterline while the basins are below it.
 * Still seed-independent (one fixed asteroid) and integer end-to-end (cross-platform
 * byte-identical, like v4). Amplitude (+/-WG_RELIEF5_AMP) stays inside the resident
 * vertical band (world.h WORLD_BAND_HALF=4 -> +/-64 vox), so a basin floor and the
 * highland beside it are both resident. */
#define WG_RELIEF5_AMP        40    /* max in/out surface displacement, voxels (x0.5m = 20 m) */
#define WG_RELIEF5_PERIOD_LOG2 8u   /* continent lattice = 2^8 = 256 vox (128 m)              */
#define WG_RELIEF5_OCTAVES    2u    /* 256 + 128 vox features (broad continents + coastlines) */
/* Sea level: a radius a little under the mean R. Terrain whose displaced surface is
 * BELOW this is an ocean basin (its air, up to this radius, is where the sea fills);
 * surface ABOVE it is dry land. R-6 leaves the pole-flat spawn (surface == R) a
 * comfortable 6 vox (3 m) above the waterline so the forge never floods, and makes
 * ~25% of the planet ocean (broad basins ~11 m deep at the lowest). The SEA itself
 * is NOT generated here (worldgen stays a pure, dry, regenerate-from-seed function);
 * a live overlay in main.c fills basin air up to a rising level (capped at WG_SEA_R). */
#define WG_SEA_R             (WG_PLANET_R - 6)   /* ocean surface radius (506 vox)            */

/* ---- River VALLEYS (0.5 gen v6: steep incised channels for the water CA) ----- *
 * v5's drainage relief is smooth + gentle (~1:5 grade), too shallow for binary-fill
 * water to flow - a spring just pools on the flat runs (measured). v6 INCISES a
 * network of valleys into it: along the zero-contours of a separate value-noise
 * field (which form connected curves winding across the surface), the surface is cut
 * DOWN by a V-profiled depth. Crucially the depth ramps DEEPER where the base terrain
 * is lower (toward the basins), so a valley FLOOR plunges from the highlands down to
 * the sea far steeper than the gentle base - giving the CA a confined, steep path
 * with frequent 1-vox steps to cascade down. Depth is band-capped (WG_VALLEY_MAXD)
 * so a valley floor never drops out of the resident vertical band. Pure integer +
 * seed-independent like the rest of worldgen. */
#define WG_VALLEY_SEED         0x5EA12345u  /* fixed: valley network is seed-independent     */
#define WG_VALLEY_PERIOD_LOG2  7u           /* valley-network lattice = 2^7 = 128 vox spacing */
#define WG_VALLEY_HALFWIDTH    420          /* |noise| band (of 2048) counted as in-valley    */
#define WG_VALLEY_FLOOR        3            /* minimum incision depth at a valley centre      */
#define WG_VALLEY_VREF         14           /* base offset above which valleys vanish (peaks) */
#define WG_VALLEY_MAXD         20           /* max incision (band-safe: <(64-AMP5_effective)) */

/* ---- Value-noise lattice (binding; cheap integer hills) -------------------- *
 * Heights are sampled on a coarse square lattice of period WG_NOISE_PERIOD
 * world-voxels (a power of two so the in-cell interpolation fraction is a mask,
 * not a divide). Lattice corner values come from an integer hash of (seed,
 * latticeX, latticeZ); a column's height bilinearly interpolates the 4
 * surrounding corners. WG_OCTAVES octaves (period halving, amplitude halving)
 * add the rolling detail. Powers of two throughout -> all shifts/masks. */
#define WG_NOISE_PERIOD  32u   /* base lattice spacing in world voxels (2^5)    */
#define WG_OCTAVES       2u    /* 1..3; more = bumpier, costlier. 2 is plenty.  */

/* ======================================================================== *
 *  INTEGER VALUE-NOISE PRIMITIVES (inline; no float, no libc math)          *
 * ======================================================================== */
/* Hash a 2D lattice corner + seed to a well-mixed 32-bit value. Reuses the
 * key_hash finalizer family (same avalanche the chunk hash uses) so worldgen
 * carries no second hash implementation. Pure integer; identical on every
 * compiler. Exposed inline so worldgen.c and tests share one definition. */
static inline uint32_t wg_hash2(uint64_t seed, int32_t lx, int32_t lz)
{
    uint64_t k = seed;
    k ^= (uint64_t)((uint32_t)lx) * 0x9E3779B97F4A7C15ull;
    k ^= (uint64_t)((uint32_t)lz) * 0xC2B2AE3D27D4EB4Full;
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* ======================================================================== *
 *  PUBLIC API  (pure C, no GL, no OS - the streaming engine + tests use it) *
 * ======================================================================== */

/* The deterministic surface height (world-Y of the first AIR voxel) for world
 * column (wx, wz), under `seed`. Result is clamped to [WG_HEIGHT_MIN,
 * WG_HEIGHT_MAX]. Pure integer; depends ONLY on (seed, wx, wz). This is the
 * heart of the determinism contract and the unit tests assert it directly. */
int  worldgen_height(uint64_t seed, int wx, int wz);

/* The radial surface displacement (voxels) at world offset-from-centre (dx,dy,dz):
 * the amount the terrain pushes the sphere surface OUT (+) or IN (-) in that
 * direction, in [-WG_RELIEF_AMP, +WG_RELIEF_AMP]. Pure integer + deterministic;
 * 0 on the Y axis (pole-flattened) and blending to full relief by sqrt(WG_POLE_
 * FLAT_R2) out. worldgen_fill_chunk adds this to WG_PLANET_R to place the crust.
 * Exposed for the worldgen unit test. */
int  worldgen_radial_offset(int dx, int dy, int dz);

/* v5 DRAINAGE variant of the above: the continent-scale surface displacement (the
 * amount the terrain pushes the sphere surface out (+) / in (-)) in that direction,
 * in [-WG_RELIEF5_AMP, +WG_RELIEF5_AMP]. Same pure-integer, pole-flattened, per-
 * direction height-field shape as worldgen_radial_offset, just at the larger
 * WG_RELIEF5_* lattice/amplitude. gen_fill_v5 adds it to WG_PLANET_R; exposed for
 * the worldgen unit test. */
int  worldgen_radial_offset_v5(int dx, int dy, int dz);

/* v6 DRAINAGE + VALLEYS variant: worldgen_radial_offset_v5 with a river-valley network
 * INCISED into it (see WG_VALLEY_* above). Same pure-integer, per-direction height-field
 * shape; the result is <= the v5 offset everywhere (valleys only cut down). gen_fill_v6
 * adds it to WG_PLANET_R; exposed for the worldgen unit test + the terrain sampler. */
int  worldgen_radial_offset_v6(int dx, int dy, int dz);

/* ---- Per-world generator versioning (cross-version save compatibility) ----- *
 * A world is pinned to the generator version it was created with (stored in its
 * save). On load the engine SELECTS that version so unmodified chunks regenerate
 * as the world they were; the build retains every supported generator. New worlds
 * use WG_GEN_VERSION (the latest). worldgen_fill_chunk + worldgen_chunk_all_air
 * dispatch on the active selection. Single world at a time -> a static selection. */
void     worldgen_select_version(uint32_t gen_version);  /* set the active world's generator */
uint32_t worldgen_active_version(void);                  /* the currently-selected version    */
int      worldgen_version_supported(uint32_t gen_version); /* 1 iff this build retains it      */

/* Fill chunk c's voxels[] for chunk coords (cx,cy,cz) from `seed`, the
 * deterministic gen(seed, coords) -> Chunk of Section 7/8. For each of the 256
 * columns: compute worldgen_height once, then fill the chunk's 16 rows by a
 * world_y compare (air above the surface; dirt in the top WG_DIRT_DEPTH solid
 * rows; stone below). Sets c->cx/cy/cz and raises CHUNK_DIRTY_MESH|CHUNK_GEN
 * (matching chunk_gen_flat). Does NOT light or mesh (the caller's mesh-upload
 * step does, after neighbours are wired). This is the WorldGenFn the WorldStore
 * binds as its gen callback. Calling twice with identical inputs yields
 * byte-identical voxels[] (the regenerate-from-seed guarantee). */
void worldgen_fill_chunk(Chunk *c, int cx, int cy, int cz, uint64_t seed);

/* 0.5 M1 sparse-air: returns 1 iff chunk (cx,cy,cz) is WHOLLY AIR under the
 * generator - i.e. even its nearest voxel to the planet centre is outside the
 * radius (min-corner squared distance > R^2), so worldgen_fill_chunk would write
 * all MAT_AIR. Lets world_insert skip the slab + the fill for the 72% of a
 * resident window that is empty space (it sets the chunk uniform-air instead).
 * Pure integer, seed-independent (one fixed asteroid), and EXACTLY consistent
 * with worldgen_fill_chunk's d2 > R2 test, so it never mis-tags a chunk that
 * would have held a single solid voxel. */
int  worldgen_chunk_all_air(int cx, int cy, int cz);

#endif /* WORLDGEN_H */
