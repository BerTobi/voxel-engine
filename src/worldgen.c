/* worldgen.c - Deterministic, seed-based procedural terrain.
 *
 * Implements the contract in worldgen.h: a pure deterministic
 * gen(seed, cx, cy, cz) -> Chunk built on cheap INTEGER value noise. Same
 * (seed, wx, wz) -> identical height; same (seed, cx, cy, cz) -> byte-identical
 * voxels[]. This is the regenerate-from-seed engine the streaming milestone
 * leans on (ARCHITECTURE.md Section 7): an evicted chunk regenerated on
 * re-entry is indistinguishable from the original, with ZERO disk I/O.
 *
 * COST DISCIPLINE (Pentium M, single core): pure integer, no float in the hot
 * path, no libc pow/sin. A column's height is a few wg_hash2 corner samples +
 * integer bilinear interpolation, summed over WG_OCTAVES octaves. The lattice
 * periods are powers of two, so the in-cell fraction is a mask and the
 * normalize is a shift - NO divides on the hot path. Integer-only means no
 * FP-rounding drift between the Linux dev host and the XP MinGW target, which
 * is the load-bearing determinism guarantee.
 *
 * No GL, no OS. Pure C99.
 */
#include "worldgen.h"
#include "chunk.h"
#include "voxel.h"
#include "material.h"

/* Ambient temperature every fresh solid voxel starts at: ~20 C -> code 60.
 * Encoded through the binding codec exactly as chunk_gen_flat does, so the
 * worked anchor (20 C -> code 60, Section 2.2) holds rather than being baked. */
#define WG_AMBIENT_C 20.0

/* log2(WG_NOISE_PERIOD): WG_NOISE_PERIOD is a power of two (32 = 2^5), so the
 * in-cell fraction is (coord & (period-1)) and the bilinear normalize is a
 * right shift by this many bits - no divide. _Static_assert below guards it. */
#define WG_NOISE_PERIOD_LOG2 5u

_Static_assert((1u << WG_NOISE_PERIOD_LOG2) == WG_NOISE_PERIOD,
               "WG_NOISE_PERIOD_LOG2 must equal log2(WG_NOISE_PERIOD)");
_Static_assert((WG_NOISE_PERIOD & (WG_NOISE_PERIOD - 1u)) == 0u,
               "WG_NOISE_PERIOD must be a power of two (mask/shift interp)");
_Static_assert(WG_OCTAVES >= 1u && WG_OCTAVES <= 3u,
               "WG_OCTAVES must be 1..3 (cost discipline)");

/* Arithmetic-shift-safe floor of a signed value by a power-of-two divisor.
 * For lattice indexing we need floor(coord / period) and the in-period offset
 * for NEGATIVE world coordinates too (the world spans both signs of X/Z).
 * C99 right-shift of a negative signed int is implementation-defined, and
 * truncating integer division rounds toward zero (wrong for negatives), so do
 * the floor explicitly with a branch on sign - still pure integer, still
 * deterministic across compilers. */
static int32_t wg_floordiv_pow2(int32_t v, uint32_t log2div)
{
    int32_t div = (int32_t)(1u << log2div);
    int32_t q = v / div;
    if ((v % div) != 0 && ((v < 0) != (div < 0)))
        --q;            /* round toward negative infinity */
    return q;
}

/* The in-period offset 0..(period-1) for a (possibly negative) world coord,
 * matching wg_floordiv_pow2: frac = v - floor(v/period)*period. Pure integer,
 * always in [0, period). */
static int32_t wg_mod_pow2(int32_t v, uint32_t log2div)
{
    int32_t period = (int32_t)(1u << log2div);
    int32_t lattice = wg_floordiv_pow2(v, log2div);
    return v - lattice * period;
}

/* One octave of integer value noise at world (wx, wz), on a lattice of the
 * given log2 period. Returns a value in a fixed-point range:
 *   - sample the 4 lattice corner hashes, reduced to a signed amplitude band
 *     [-2048, +2047] (12-bit signed) so the bilinear products stay well inside
 *     int32 (max |corner|*period*period ~ 2048*32*32 = 2^21, comfortably under
 *     2^31), and so the octave sum keeps headroom.
 *   - bilinearly interpolate across the in-cell fraction (fx, fz), each in
 *     [0, period), normalizing by a single right shift of (2*log2period).
 * Pure integer; no divide (period is a power of two). */
static int32_t wg_octave(uint64_t seed, int32_t wx, int32_t wz, uint32_t log2period)
{
    int32_t period = (int32_t)(1u << log2period);
    int32_t lx = wg_floordiv_pow2(wx, log2period);   /* lattice cell X */
    int32_t lz = wg_floordiv_pow2(wz, log2period);   /* lattice cell Z */
    int32_t fx = wg_mod_pow2(wx, log2period);        /* in-cell X, 0..period-1 */
    int32_t fz = wg_mod_pow2(wz, log2period);        /* in-cell Z, 0..period-1 */

    /* Corner amplitudes: take the top 12 bits of the well-mixed hash and recenter
     * to signed [-2048, +2047]. Top bits are the best-mixed of wg_hash2. */
    int32_t c00 = (int32_t)(wg_hash2(seed, lx,     lz)     >> 20) - 2048;
    int32_t c10 = (int32_t)(wg_hash2(seed, lx + 1, lz)     >> 20) - 2048;
    int32_t c01 = (int32_t)(wg_hash2(seed, lx,     lz + 1) >> 20) - 2048;
    int32_t c11 = (int32_t)(wg_hash2(seed, lx + 1, lz + 1) >> 20) - 2048;

    /* Bilinear blend with integer weights. fx,fz are the numerators over
     * `period`; combine then normalize by >> (2*log2period) in one shot. */
    int32_t inv_fx = period - fx;
    int32_t inv_fz = period - fz;

    int32_t top    = c00 * inv_fx + c10 * fx;   /* lerp along X at z = lz   */
    int32_t bottom = c01 * inv_fx + c11 * fx;   /* lerp along X at z = lz+1 */
    int32_t v      = top * inv_fz + bottom * fz;

    return v >> (2u * log2period);              /* back to the [-2048,2047] band */
}

/* Deterministic surface height (world-Y of the first AIR voxel) for column
 * (wx, wz) under `seed`, clamped to [WG_HEIGHT_MIN, WG_HEIGHT_MAX]. Pure
 * integer; depends ONLY on the arguments (no globals/statics/time/RNG). This
 * is the heart of the determinism contract.
 *
 * Sum WG_OCTAVES octaves with period-halving and amplitude-halving (classic
 * fractal value noise). Octave 0 carries full weight; each later octave is
 * halved. The accumulated value sits in the [-2048, +2047] fixed-point band;
 * map it to a height offset in [-WG_HEIGHT_AMP/2, +WG_HEIGHT_AMP/2] about
 * WG_SEA_LEVEL, then clamp. All shifts/masks - no divide on this path. */
int worldgen_height(uint64_t seed, int wx, int wz)
{
    int32_t acc = 0;          /* weighted octave sum, fixed-point band */
    int32_t weight_total = 0; /* total amplitude used, for normalization */
    uint32_t oct;

    for (oct = 0u; oct < WG_OCTAVES; ++oct) {
        /* period halves each octave: log2period = base - oct (clamped >= 0 so
         * the finest octave never collapses below period 1). amplitude halves
         * each octave: weight = 1 << (WG_OCTAVES-1-oct). */
        uint32_t log2period = (WG_NOISE_PERIOD_LOG2 > oct)
                            ? (WG_NOISE_PERIOD_LOG2 - oct) : 0u;
        int32_t weight = (int32_t)(1u << (WG_OCTAVES - 1u - oct));
        acc += wg_octave(seed, (int32_t)wx, (int32_t)wz, log2period) * weight;
        weight_total += weight;
    }

    /* Normalize the weighted sum back into the [-2048, +2047] band, then scale
     * to a +/- (WG_HEIGHT_AMP/2) offset about sea level. acc/weight_total is in
     * [-2048,2047]; multiply by the half-amplitude and shift down by 11 (the
     * band is 12-bit signed, 2^11 = 2048). weight_total is a small power-of-two
     * sum but not necessarily a single power of two, so this one division by
     * weight_total is the ONLY divide - and it is off the per-voxel hot path
     * (called once per column, 256 times per chunk, not per voxel). */
    {
        int32_t band   = acc / weight_total;                 /* [-2048, 2047] */
        int32_t offset = (band * (WG_HEIGHT_AMP / 2)) >> 11; /* +/- amp/2     */
        int height = WG_SEA_LEVEL + (int)offset;

        if (height < WG_HEIGHT_MIN) height = WG_HEIGHT_MIN;
        if (height > WG_HEIGHT_MAX) height = WG_HEIGHT_MAX;
        return height;
    }
}

/* Fill chunk c's voxels[] for chunk coords (cx,cy,cz) from `seed`: the
 * deterministic gen(seed, coords) -> Chunk of Section 7/8. For each of the 256
 * columns compute worldgen_height ONCE, then fill the 16 rows by a world_y
 * compare - air at/above the surface; dirt in the top WG_DIRT_DEPTH solid rows;
 * stone below. Same strata shape and fill=15/ambient-temp stamping as
 * chunk_gen_flat. Sets c->cx/cy/cz and raises CHUNK_DIRTY_MESH|CHUNK_GEN.
 *
 * Determinism: a pure function of (cx,cy,cz,seed) into c->voxels (plus the
 * coord/flag stamp). Calling twice with identical inputs yields byte-identical
 * voxels[] (the regenerate-from-seed guarantee). */
void worldgen_fill_chunk(Chunk *c, int cx, int cy, int cz, uint64_t seed)
{
    uint8_t ambient = temp_encode_c(WG_AMBIENT_C);
    int lx, ly, lz;
    /* Squared radii (pure integer -> deterministic, no sqrt, no FP drift). */
    const long R2  = (long)WG_PLANET_R * (long)WG_PLANET_R;
    const long Rd  = (long)WG_PLANET_R - (long)WG_DIRT_DEPTH;
    const long Rd2 = Rd * Rd;

    (void)seed;   /* one fixed asteroid; generation is seed-independent */

    c->cx = cx;
    c->cy = cy;
    c->cz = cz;

    /* Spherical asteroid: STONE where world-distance^2 from the center <= R^2,
     * a WG_DIRT_DEPTH dirt crust just under the surface, AIR outside. lx is the
     * fastest (contiguous, vox_index = lx + ly*16 + lz*256). */
    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        long dz = (long)(cz * CHUNK_DIM + lz) - WG_PLANET_CZ;
        for (ly = 0; ly < CHUNK_DIM; ++ly) {
            long dy = (long)(cy * CHUNK_DIM + ly) - WG_PLANET_CY;
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                long dx = (long)(cx * CHUNK_DIM + lx) - WG_PLANET_CX;
                long d2 = dx * dx + dy * dy + dz * dz;
                Voxel v = 0;

                if (d2 > R2) {
                    vox_set_mat(&v, MAT_AIR);
                } else if (d2 > Rd2) {
                    vox_set_mat(&v, MAT_DIRT);     /* crust */
                    vox_set_fill(&v, 15);
                } else {
                    vox_set_mat(&v, MAT_STONE);    /* core */
                    vox_set_fill(&v, 15);
                }

                vox_set_temp_code(&v, ambient);
                c->voxels[vox_index(lx, ly, lz)] = v;
            }
        }
    }

    c->flags |= CHUNK_DIRTY_MESH | CHUNK_GEN;
}
