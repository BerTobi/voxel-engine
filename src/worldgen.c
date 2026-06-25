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

/* ======================================================================== *
 *  RADIAL TERRAIN RELIEF (0.5) - displace the sphere surface per direction   *
 * ======================================================================== */

/* 3D lattice-corner hash - the wg_hash2 finalizer family with a third axis. */
static uint32_t wg_hash3(uint32_t seed, int32_t lx, int32_t ly, int32_t lz)
{
    uint64_t k = (uint64_t)seed;
    k ^= (uint64_t)((uint32_t)lx) * 0x9E3779B97F4A7C15ull;
    k ^= (uint64_t)((uint32_t)ly) * 0xC2B2AE3D27D4EB4Full;
    k ^= (uint64_t)((uint32_t)lz) * 0x165667B19E3779F9ull;
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* Floor of the integer square root of n>=0 (bit-by-bit, pure integer, identical
 * on every compiler - no float, so no cross-platform FP drift). */
static int32_t wg_isqrt(int64_t n)
{
    int64_t x = 0, b = 1;
    if (n <= 0) return 0;
    while (b * 4 <= n) b *= 4;            /* highest power of four <= n */
    while (b > 0) {
        if (n >= x + b) { n -= x + b; x = (x >> 1) + b; }
        else            { x >>= 1; }
        b >>= 2;
    }
    return (int32_t)x;
}

/* One octave of integer 3D value noise at (x,y,z) on a lattice of the given log2
 * period: the 8 cube-corner hashes reduced to the signed [-2048,2047] band, then
 * TRILINEARLY interpolated (int64 - period^3 * 2048 exceeds int32 for log2>=7).
 * Normalised back to the band by >> (3*log2period). Pure integer, no divide. */
static int32_t wg_octave3(uint32_t seed, int32_t x, int32_t y, int32_t z, uint32_t log2period)
{
    int32_t period = (int32_t)(1u << log2period);
    int32_t lx = wg_floordiv_pow2(x, log2period), fx = wg_mod_pow2(x, log2period);
    int32_t ly = wg_floordiv_pow2(y, log2period), fy = wg_mod_pow2(y, log2period);
    int32_t lz = wg_floordiv_pow2(z, log2period), fz = wg_mod_pow2(z, log2period);
    int32_t ifx = period - fx, ify = period - fy, ifz = period - fz;
    int32_t c000 = (int32_t)(wg_hash3(seed, lx,     ly,     lz)     >> 20) - 2048;
    int32_t c100 = (int32_t)(wg_hash3(seed, lx + 1, ly,     lz)     >> 20) - 2048;
    int32_t c010 = (int32_t)(wg_hash3(seed, lx,     ly + 1, lz)     >> 20) - 2048;
    int32_t c110 = (int32_t)(wg_hash3(seed, lx + 1, ly + 1, lz)     >> 20) - 2048;
    int32_t c001 = (int32_t)(wg_hash3(seed, lx,     ly,     lz + 1) >> 20) - 2048;
    int32_t c101 = (int32_t)(wg_hash3(seed, lx + 1, ly,     lz + 1) >> 20) - 2048;
    int32_t c011 = (int32_t)(wg_hash3(seed, lx,     ly + 1, lz + 1) >> 20) - 2048;
    int32_t c111 = (int32_t)(wg_hash3(seed, lx + 1, ly + 1, lz + 1) >> 20) - 2048;
    {
        int64_t x00 = (int64_t)c000 * ifx + (int64_t)c100 * fx;   /* lerp X, y0 z0 */
        int64_t x10 = (int64_t)c010 * ifx + (int64_t)c110 * fx;   /* lerp X, y1 z0 */
        int64_t x01 = (int64_t)c001 * ifx + (int64_t)c101 * fx;   /* lerp X, y0 z1 */
        int64_t x11 = (int64_t)c011 * ifx + (int64_t)c111 * fx;   /* lerp X, y1 z1 */
        int64_t y0  = x00 * ify + x10 * fy;                       /* lerp Y, z0    */
        int64_t y1  = x01 * ify + x11 * fy;                       /* lerp Y, z1    */
        int64_t v   = y0 * ifz + y1 * fz;                         /* lerp Z        */
        return (int32_t)(v >> (3u * log2period));                 /* -> [-2048,2047] */
    }
}

int worldgen_radial_offset(int dx, int dy, int dz)
{
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
    int64_t horiz2;
    int32_t d, sx, sy, sz, acc = 0, wtot = 0, band, off;
    uint32_t oct;
    if (d2 == 0) return 0;
    d = wg_isqrt(d2);
    if (d == 0) return 0;
    /* Project the direction onto the sphere of radius R: a PER-DIRECTION sample
     * point, so all voxels in a radial column see ~the same displacement (a clean
     * height field, not a 3D density that would carve caves). Integer divide. */
    sx = (int32_t)((int64_t)dx * WG_PLANET_R / d);
    sy = (int32_t)((int64_t)dy * WG_PLANET_R / d);
    sz = (int32_t)((int64_t)dz * WG_PLANET_R / d);
    for (oct = 0u; oct < WG_RELIEF_OCTAVES; ++oct) {              /* fractal sum */
        uint32_t lp = (WG_RELIEF_PERIOD_LOG2 > oct) ? (WG_RELIEF_PERIOD_LOG2 - oct) : 0u;
        int32_t  w  = (int32_t)(1u << (WG_RELIEF_OCTAVES - 1u - oct));
        acc  += wg_octave3(WG_RELIEF_SEED, sx, sy, sz, lp) * w;
        wtot += w;
    }
    band = acc / wtot;                                           /* [-2048,2047] */
    off  = (int32_t)(((int64_t)band * WG_RELIEF_AMP) >> 11);     /* +/- WG_RELIEF_AMP */
    /* Pole-flatten: scale the displacement to 0 within sqrt(WG_POLE_FLAT_R2) of the
     * Y axis, so the spawn pole / forge / chimney sit on flat, predictable crust. */
    horiz2 = (int64_t)dx * dx + (int64_t)dz * dz;
    if (horiz2 < WG_POLE_FLAT_R2)
        off = (int32_t)((int64_t)off * horiz2 / WG_POLE_FLAT_R2);
    return off;
}

/* Fill chunk c's voxels[] for chunk coords (cx,cy,cz) from `seed`: the
 * deterministic gen(seed, coords) -> Chunk of Section 7/8. A spherical asteroid
 * whose surface RADIUS is displaced per direction by worldgen_radial_offset
 * (hills/ridges/basins), with a WG_DIRT_DEPTH dirt crust. The displacement is only
 * evaluated in the thin transition shell [R-AMP-DIRT, R+AMP]; voxels deeper than
 * that are unconditionally STONE and voxels beyond it AIR (so the per-voxel noise
 * cost is paid only near the surface). lx is the fastest axis.
 *
 * Determinism: a pure function of (cx,cy,cz) into c->voxels (seed-independent, one
 * fixed asteroid). Calling twice with identical inputs yields byte-identical
 * voxels[] (the regenerate-from-seed guarantee, asserted cross-platform). */
/* gen v3 (RETAINED for cross-version save compat): the smooth sphere - STONE where
 * d^2 <= R^2, a WG_DIRT_DEPTH dirt crust, AIR outside. No relief. A world stamped
 * gen_version 3 keeps loading as the smooth ball it was created as. */
static void gen_fill_v3(Chunk *c, int cx, int cy, int cz)
{
    uint8_t ambient = temp_encode_c(WG_AMBIENT_C);
    int lx, ly, lz;
    const long R2  = (long)WG_PLANET_R * (long)WG_PLANET_R;
    const long Rd  = (long)WG_PLANET_R - (long)WG_DIRT_DEPTH;
    const long Rd2 = Rd * Rd;
    c->cx = cx; c->cy = cy; c->cz = cz;
    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        long dz = (long)(cz * CHUNK_DIM + lz) - WG_PLANET_CZ;
        for (ly = 0; ly < CHUNK_DIM; ++ly) {
            long dy = (long)(cy * CHUNK_DIM + ly) - WG_PLANET_CY;
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                long dx = (long)(cx * CHUNK_DIM + lx) - WG_PLANET_CX;
                long d2 = dx * dx + dy * dy + dz * dz;
                Voxel v = 0;
                uint8_t mat = (d2 > R2) ? MAT_AIR : (d2 > Rd2) ? MAT_DIRT : MAT_STONE;
                vox_set_mat(&v, mat);
                if (mat != MAT_AIR) vox_set_fill(&v, 15);
                vox_set_temp_code(&v, ambient);
                c->voxels[vox_index(lx, ly, lz)] = v;
            }
        }
    }
    c->flags |= CHUNK_DIRTY_MESH | CHUNK_GEN;
}

/* gen v4 (current): the smooth sphere with radial terrain RELIEF (hills/basins). */
static void gen_fill_v4(Chunk *c, int cx, int cy, int cz)
{
    uint8_t ambient = temp_encode_c(WG_AMBIENT_C);
    int lx, ly, lz;
    /* The displaced surface lies in the shell [R-AMP-DIRT, R+AMP]; outside it the
     * classification is unconditional (deep core STONE, far space AIR), so the
     * per-voxel relief noise is evaluated ONLY in that thin shell. Pure integer. */
    const long Rhi  = (long)WG_PLANET_R + WG_RELIEF_AMP;
    const long Rhi2 = Rhi * Rhi;
    const long Rlo  = (long)WG_PLANET_R - WG_RELIEF_AMP - WG_DIRT_DEPTH;
    const long Rlo2 = (Rlo > 0) ? Rlo * Rlo : 0;

    c->cx = cx;
    c->cy = cy;
    c->cz = cz;

    /* Spherical asteroid with radial relief: the surface radius in a given
     * direction is R + worldgen_radial_offset(dir). STONE below it (with a
     * WG_DIRT_DEPTH dirt crust just under the surface), AIR above. lx is the
     * fastest (contiguous, vox_index = lx + ly*16 + lz*256). */
    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        long dz = (long)(cz * CHUNK_DIM + lz) - WG_PLANET_CZ;
        for (ly = 0; ly < CHUNK_DIM; ++ly) {
            long dy = (long)(cy * CHUNK_DIM + ly) - WG_PLANET_CY;
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                long dx = (long)(cx * CHUNK_DIM + lx) - WG_PLANET_CX;
                long d2 = dx * dx + dy * dy + dz * dz;
                Voxel v = 0;
                uint8_t mat;

                if (d2 <= Rlo2) {
                    mat = MAT_STONE;                /* below any possible surface */
                } else if (d2 > Rhi2) {
                    mat = MAT_AIR;                  /* above any possible surface */
                } else {
                    int  off  = worldgen_radial_offset((int)dx, (int)dy, (int)dz);
                    long Re   = (long)WG_PLANET_R + off;          /* displaced surface */
                    long Re2  = Re * Re;
                    long Rde  = Re - WG_DIRT_DEPTH;
                    long Rde2 = (Rde > 0) ? Rde * Rde : 0;
                    if (d2 > Re2)        mat = MAT_AIR;
                    else if (d2 > Rde2)  mat = MAT_DIRT;          /* crust */
                    else                 mat = MAT_STONE;         /* core  */
                }

                vox_set_mat(&v, mat);
                if (mat != MAT_AIR) vox_set_fill(&v, 15);
                vox_set_temp_code(&v, ambient);
                c->voxels[vox_index(lx, ly, lz)] = v;
            }
        }
    }

    c->flags |= CHUNK_DIRTY_MESH | CHUNK_GEN;
}

/* ======================================================================== *
 *  PER-WORLD GENERATOR VERSIONING (cross-version save compatibility)         *
 * ======================================================================== *
 * A world is pinned to the generator version it was CREATED with: its save
 * stores that gen_version, and on load the engine selects the matching generator
 * here and regenerates unmodified chunks with IT - so a world keeps loading as the
 * world it was, even after a later version changes the default terrain. The build
 * RETAINS every supported generator (below); changing terrain in a future version
 * means ADDING a gen_fill_vN and a case, never editing an old one (an old generator
 * must keep producing byte-identical output forever or its worlds would corrupt).
 * Single active version (one world at a time, single-threaded) -> a static is fine
 * and deterministic. The MP handshake advertises the WORLD's version, not the
 * build's, so peers regenerate identically (or are refused if they lack it). */
static uint32_t g_gen_version = WG_GEN_VERSION;   /* the active world's generator */

void worldgen_select_version(uint32_t v) { g_gen_version = v; }
uint32_t worldgen_active_version(void)   { return g_gen_version; }
int worldgen_version_supported(uint32_t v) { return v == 3u || v == 4u; }

void worldgen_fill_chunk(Chunk *c, int cx, int cy, int cz, uint64_t seed)
{
    (void)seed;   /* one fixed asteroid; generation is seed-independent */
    switch (g_gen_version) {
        case 3u:           gen_fill_v3(c, cx, cy, cz); break;
        case 4u: default:  gen_fill_v4(c, cx, cy, cz); break;
    }
}

/* The nearest distance, along one axis, from the planet centre C to the chunk's
 * world-coord span [base, base+CHUNK_DIM-1]: 0 if C is inside the span, else the
 * gap to the nearer end. */
static long axis_near(int base, int center)
{
    int lo = base, hi = base + CHUNK_DIM - 1;
    if (center < lo) return (long)(lo - center);
    if (center > hi) return (long)(center - hi);
    return 0;
}

int worldgen_chunk_all_air(int cx, int cy, int cz)
{
    /* Min squared distance from the centre to ANY voxel in the chunk = sum of the
     * per-axis nearest distances squared. If even that is beyond the MAXIMUM
     * possible surface radius, every voxel is air. Must be conservative against the
     * peak (a v4 relief ridge reaches R+AMP) or sparse-air would drop real terrain;
     * v3 (smooth) has no relief so its max surface is exactly R. Dispatch on the
     * active world's generator version, matching worldgen_fill_chunk. */
    const long Rhi = (long)WG_PLANET_R + ((g_gen_version >= 4u) ? WG_RELIEF_AMP : 0);
    const long Rhi2 = Rhi * Rhi;
    long nx = axis_near(cx * CHUNK_DIM, WG_PLANET_CX);
    long ny = axis_near(cy * CHUNK_DIM, WG_PLANET_CY);
    long nz = axis_near(cz * CHUNK_DIM, WG_PLANET_CZ);
    return (nx * nx + ny * ny + nz * nz) > Rhi2;
}
