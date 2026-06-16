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
#define WG_GEN_VERSION   2u   /* 2: spherical asteroid (was 1: rolling heightmap) */

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
#define WG_DIRT_DEPTH    3

/* ---- Spherical asteroid (0.3 radial-gravity prototype) -------------------- *
 * The world is a single solid voxel BALL: a voxel is STONE where its world-space
 * distance from the center WG_PLANET_C* is <= WG_PLANET_R, with a WG_DIRT_DEPTH-
 * thick DIRT crust just beneath the surface, and AIR outside. Player gravity
 * (player.c) and the camera (main.c) point at this SAME center, so "down" is
 * radial everywhere. This replaces the rolling heightmap for the prototype; the
 * fill is a pure-integer squared-distance test, so determinism is preserved (and
 * it is independent of `seed` - there is one fixed asteroid). Center is chunk-
 * interior-ish at the HOME column (DEMO_WORLD_X/Z = 8) and lifted to world-Y 28 so
 * the whole ball sits in the resident vertical band (see world.h WORLD_BAND_Y1). */
#define WG_PLANET_CX     8     /* planet center, world voxels (X)                */
#define WG_PLANET_CY     28    /* planet center, world voxels (Y)                */
#define WG_PLANET_CZ     8     /* planet center, world voxels (Z)                */
#define WG_PLANET_R      28    /* planet radius, world voxels (~40 s lap)        */

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

#endif /* WORLDGEN_H */
