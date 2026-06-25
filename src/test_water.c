/* test_water.c - 0.5 water CA harness slot.
 *
 * M0: a GL-free DETERMINISM check on the EXISTING liquid machinery (the
 * fill-nibble fluid pass that ships since the original M6) - two identical water
 * worlds ticked the same N must hash-equal via sim_state_hash (which covers the
 * fill nibble). This pins the determinism floor the binary-fill water CA will
 * build on. M3 expands this into the real flow / settle / conservation
 * assertions, seeded by scratchpad/water_ca_proto.c (the binary-fill prototype).
 *
 * Compiled with -DVOXEL_DETERMINISM_HARNESS (sim_state_hash is guarded by it),
 * mirroring testdeterminism. Links material.c chunk.c sim.c progress.c. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* 0.5 M1: calloc for test chunk blocks */
#include "voxel.h"
#include "chunk.h"
#include "material.h"
#include "sim.h"

static int fails = 0;
#define CHECK(c,msg) do{ if(c) printf("PASS: %s\n",msg); else {printf("FAIL: %s\n",msg); fails++;} }while(0)

/* a small water world: a stone floor with a raised water blob above it that the
 * existing fluid pass will move - the point is reproducibility, not the shape. */
static void setup_water(SimState *s, Chunk *c)
{
    int x, y, z;
    Voxel stone = 0, water = 0;
    memset(c, 0, sizeof *c);
    c->voxels = calloc(CHUNK_VOXELS, sizeof(Voxel));   /* 0.5 M1: voxels is a pointer now */
    c->slab_idx = -1;
    vox_set_mat(&stone, MAT_STONE); vox_set_fill(&stone, FLUID_FULL); vox_set_temp_code(&stone, 60u);
    vox_set_mat(&water, MAT_WATER); vox_set_fill(&water, FLUID_FULL); vox_set_temp_code(&water, 60u);
    for (z = 0; z < CHUNK_DIM; ++z) for (x = 0; x < CHUNK_DIM; ++x) for (y = 0; y < 2; ++y)
        c->voxels[vox_index(x, y, z)] = stone;                 /* floor y0..1 */
    for (z = 6; z < 10; ++z) for (x = 6; x < 10; ++x) for (y = 8; y < 12; ++y)
        c->voxels[vox_index(x, y, z)] = water;                 /* raised blob */
    sim_build_conduct_lut();
    sim_init(s, c);
}

int main(void)
{
    SimState sa, sb;
    static Chunk ca, cb;   /* static: keep ~16 KiB each off the stack */
    int n;
    printf("== test_water (0.5 M0: liquid determinism harness slot) ==\n");
    setup_water(&sa, &ca);
    setup_water(&sb, &cb);
    for (n = 0; n < 120; ++n) { sim_tick(&sa); sim_tick(&sb); }
    CHECK(sim_state_hash(&sa) == sim_state_hash(&sb),
          "two identical water worlds hash-equal after 120 ticks (deterministic)");
    sim_shutdown(&sa);
    sim_shutdown(&sb);
    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
