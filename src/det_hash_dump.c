/* det_hash_dump.c - 0.5 M6 CROSS-PLATFORM determinism fingerprint.
 *
 * The Linux determinism harness (test_determinism.c / test_water.c) proves the CA
 * is reproducible WITHIN one platform. This dumper proves it is reproducible
 * ACROSS platforms: it runs fixed heat + water + combined scenarios, ticks each a
 * fixed number of times, and prints sim_state_hash() as stable hex. Build it both
 * native (Linux) and for Windows (i686-w64-mingw32 + -DVOXEL_DETERMINISM_HARNESS),
 * run the .exe under wine, and diff the output: identical lines == the Windows
 * build computes the world byte-for-byte like Linux (the CA is integer-only, so
 * this should hold by construction - this is the explicit verification of that).
 *
 * No GL, no platform, no sockets - drives sim_tick() directly like the harness.
 *
 * Build (native):
 *   gcc -std=c99 -Wall -Wextra -Isrc -DVOXEL_DETERMINISM_HARNESS -o build/det_dump \
 *       src/material.c src/chunk.c src/sim.c src/progress.c src/det_hash_dump.c -lm
 * Build (Windows, run under wine):
 *   i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -Isrc -DVOXEL_DETERMINISM_HARNESS \
 *       -o build/det_dump.exe <same sources>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "voxel.h"
#include "material.h"
#include "chunk.h"
#include "sim.h"
#include "worldgen.h"

/* Print a 64-bit hash as 16 hex digits using only 32-bit halves, so the format is
 * identical on LP64 (Linux) and the ILP32 Windows target (no %llx / %I64x split). */
static void emit(const char *label, uint64_t h)
{
    unsigned long hi = (unsigned long)((h >> 32) & 0xffffffffUL);
    unsigned long lo = (unsigned long)(h & 0xffffffffUL);
    printf("%s=%08lx%08lx\n", label, hi, lo);
}

static Voxel mk(uint8_t mat, uint8_t fill)
{ Voxel v = 0; vox_set_mat(&v, mat); vox_set_fill(&v, fill); vox_set_temp_code(&v, 60u); return v; }

/* All-ambient-air backing (matches worldgen + the test_water harness). */
static void back(Chunk *c)
{
    int i; Voxel air = mk(MAT_AIR, 0);
    memset(c, 0, sizeof *c);
    c->voxels = (Voxel *)calloc(CHUNK_VOXELS, sizeof(Voxel));
    c->slab_idx = -1;
    for (i = 0; i < CHUNK_VOXELS; ++i) c->voxels[i] = air;
}
static void put(Chunk *c, int x, int y, int z, Voxel v) { c->voxels[vox_index(x,y,z)] = v; }

/* HEAT: the copper-melt rig (6 lava faces around the centre), 1200 ticks. */
static uint64_t heat_hash(void)
{
    static Chunk c; SimState s; Voxel base = 0; int i, n;
    int hx = 8, hy = 8, hz = 8;
    memset(&c, 0, sizeof c);
    c.voxels = (Voxel *)calloc(CHUNK_VOXELS, sizeof(Voxel));
    c.slab_idx = -1;
    vox_set_mat(&base, MAT_COPPER); vox_set_fill(&base, FLUID_FULL); vox_set_temp_code(&base, 60u);
    for (i = 0; i < CHUNK_VOXELS; ++i) c.voxels[i] = base;
    sim_build_conduct_lut(); sim_init(&s, &c);
    for (n = 0; n < 6; ++n) {
        int nx = hx + ((n==0) - (n==1)), ny = hy + ((n==2) - (n==3)), nz = hz + ((n==4) - (n==5));
        sim_set_source(&s, (uint16_t)vox_index(nx,ny,nz), 212u /*lava*/);
    }
    for (n = 0; n < 1200; ++n) sim_tick(&s);
    { uint64_t h = sim_state_hash(&s); sim_shutdown(&s); free(c.voxels); return h; }
}

/* WATER: a 4x4x4 blob falls onto a floor and settles (binary-fill CA), 400 ticks. */
static uint64_t water_hash(void)
{
    static Chunk c; SimState s; int x,y,z,t;
    back(&c);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++) for (y=0;y<2;y++) put(&c,x,y,z, mk(MAT_STONE,15));
    for (z=6;z<10;z++) for (x=6;x<10;x++) for (y=8;y<12;y++) put(&c,x,y,z, mk(MAT_WATER,15));
    sim_build_conduct_lut(); sim_init(&s, &c);
    for (t=0;t<400;t++) sim_tick(&s);
    { uint64_t h = sim_state_hash(&s); sim_shutdown(&s); free(c.voxels); return h; }
}

/* COMBINED: heat + water in one chunk - a lava source heats while a water spring
 * pours over a floor. Exercises both passes' integer math in the same state. */
static uint64_t combined_hash(void)
{
    static Chunk c; SimState s; int x,z,t;
    back(&c);
    for (z=0;z<CHUNK_DIM;z++) for (x=0;x<CHUNK_DIM;x++) put(&c,x,0,z, mk(MAT_STONE,15));   /* floor */
    put(&c, 2,1,2, mk(MAT_COPPER, FLUID_FULL));                                            /* a block to heat */
    put(&c, 8,13,8, mk(MAT_WATER,15));                                                     /* spring cell */
    sim_build_conduct_lut(); sim_init(&s, &c);
    sim_set_source(&s, (uint16_t)vox_index(2,2,2), 212u);   /* lava above the copper block */
    sim_set_spring(&s, (uint16_t)vox_index(8,13,8), 60u);   /* inexhaustible water source  */
    for (t=0;t<500;t++) sim_tick(&s);
    { uint64_t h = sim_state_hash(&s); sim_shutdown(&s); free(c.voxels); return h; }
}

/* WORLDGEN: an equatorial SURFACE chunk (relief fully active, far from the flattened
 * pole), FNV'd byte-by-byte (little-endian on both x86 targets). Exercises the relief
 * path's integer isqrt + divide + 3D value noise - they must hash identically. */
static uint64_t worldgen_hash(void)
{
    static Chunk c;
    uint64_t h = 14695981039346656037ull;
    int i;
    c.voxels = (Voxel *)calloc(CHUNK_VOXELS, sizeof(Voxel));
    c.slab_idx = -1;
    worldgen_fill_chunk(&c, 32, 32, 0, 0);
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        uint32_t v = (uint32_t)c.voxels[i];
        int b;
        for (b = 0; b < 4; ++b) { h ^= (uint64_t)((v >> (8 * b)) & 0xffu); h *= 1099511628211ull; }
    }
    free(c.voxels);
    return h;
}

/* LEVELED: a 4x4x6 water block dropped into a walled basin with the surface-leveling
 * FINISHER enabled (down=-Y). Exercises the finisher's integer binary-search + cell
 * distribution - it must settle to a byte-identical flat state on every platform. */
static uint64_t leveled_hash(void)
{
    static Chunk c; SimState s; int x, y, z, t;
    c.voxels = (Voxel *)calloc(CHUNK_VOXELS, sizeof(Voxel));
    c.slab_idx = -1;
    for (x = 0; x < 16; ++x) for (y = 0; y < 16; ++y) for (z = 0; z < 16; ++z)
        c.voxels[vox_index(x,y,z)] = mk(MAT_AIR, 0);
    for (x = 1; x <= 6; ++x) for (z = 1; z <= 6; ++z) {
        c.voxels[vox_index(x,0,z)] = mk(MAT_STONE,15);
        c.voxels[vox_index(x,1,z)] = mk(MAT_STONE,15);
    }
    for (y = 2; y <= 15; ++y) for (x = 1; x <= 6; ++x) {
        c.voxels[vox_index(x,y,1)] = mk(MAT_STONE,15); c.voxels[vox_index(x,y,6)] = mk(MAT_STONE,15);
        c.voxels[vox_index(1,y,x)] = mk(MAT_STONE,15); c.voxels[vox_index(6,y,x)] = mk(MAT_STONE,15);
    }
    for (x = 2; x <= 5; ++x) for (z = 2; z <= 5; ++z) for (y = 8; y <= 13; ++y)
        c.voxels[vox_index(x,y,z)] = mk(MAT_WATER,15);
    sim_build_conduct_lut(); sim_init(&s, &c);
    sim_set_down_face(&s, 3);   /* enables the leveling finisher */
    for (t = 0; t < 400; ++t) { sim_tick(&s); if (s.act.count == 0) break; }
    { uint64_t h = sim_state_hash(&s); sim_shutdown(&s); free(c.voxels); return h; }
}

/* RADIAL: a spring high in a walled shaft with the RADIAL fill-and-spill finisher.
 * The chunk sits at the planet's +Y pole (world Y ~ CY+R) and a planet centre is set,
 * so sim_cell_pot takes its d2 (long) branch. Exercises the radial potential math +
 * the spring body-donation ACROSS the LP64/ILP32 boundary - d2 must not overflow or
 * differ by data model, and the donation order must be byte-identical on both. */
static uint64_t radial_hash(void)
{
    static Chunk c; SimState s; int x, y, z, t;
    c.voxels = (Voxel *)calloc(CHUNK_VOXELS, sizeof(Voxel));
    c.slab_idx = -1;
    c.cx = 0; c.cy = WG_PLANET_R / CHUNK_DIM; c.cz = 0;   /* world Y ~ CY+R: the +Y pole */
    for (x = 0; x < 16; ++x) for (y = 0; y < 16; ++y) for (z = 0; z < 16; ++z)
        put(&c, x,y,z, mk(MAT_STONE,15));
    for (z = 6; z <= 10; ++z) for (y = 1; y <= 13; ++y) for (x = 6; x <= 10; ++x)
        put(&c, x,y,z, mk(MAT_AIR,0));                    /* a shaft cavity            */
    put(&c, 8,12,8, mk(MAT_WATER,15));                    /* spring high in the shaft  */
    sim_set_planet_center(WG_PLANET_CX, WG_PLANET_CY, WG_PLANET_CZ);   /* d2 branch    */
    sim_build_conduct_lut(); sim_init(&s, &c);
    sim_set_down_face(&s, 3);
    sim_set_spring(&s, (uint16_t)vox_index(8,12,8), 60u);
    for (t = 0; t < 1500; ++t) sim_tick(&s);
    { uint64_t h = sim_state_hash(&s); sim_shutdown(&s); free(c.voxels); return h; }
}

int main(void)
{
    emit("HEAT",     heat_hash());
    emit("WATER",    water_hash());
    emit("COMBINED", combined_hash());
    emit("WORLDGEN", worldgen_hash());
    emit("LEVELED",  leveled_hash());
    emit("RADIAL",   radial_hash());
    return 0;
}
