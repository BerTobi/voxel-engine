/* test_sparse.c - 0.5 M1: sparse-air chunk storage.
 *
 * Chunk.voxels became a POINTER: a non-uniform chunk borrows a 16 KiB block from
 * the WorldStore slab sub-pool; a uniform-AIR chunk (the 72%-empty majority of a
 * resident window) holds voxels==NULL + uniform_word and borrows none. Asserts:
 *   - an exterior chunk inserts UNIFORM-air (no slab); an interior one REALIZED;
 *   - chunk_vox() reads both correctly;
 *   - a uniform chunk meshes to zero quads, a realized one to >0;
 *   - world_realize expands the uniform word; world_set_uniform returns the slab;
 *   - over a full streamed window the slab pool NEVER exhausts and the realized
 *     block count is far below the resident chunk count (the working-set win).
 * White-box: world.h exposes the full WorldStore, so we read slab_free_top /
 * slab_inuse_peak directly. GL-free. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voxel.h"
#include "chunk.h"
#include "material.h"
#include "mesher.h"
#include "worldgen.h"
#include "world.h"

static int fails = 0;
#define CHECK(c,msg) do{ if(c) printf("PASS: %s\n",msg); else {printf("FAIL: %s\n",msg); fails++;} }while(0)

static void gen_cb(Chunk *c, int cx, int cy, int cz, uint64_t seed, void *u)
{ (void)u; worldgen_fill_chunk(c, cx, cy, cz, seed); }
static int air_cb(int cx, int cy, int cz, uint64_t seed, void *u)
{ (void)seed; (void)u; return worldgen_chunk_all_air(cx, cy, cz); }

int main(void)
{
    WorldStore *ws = (WorldStore *)malloc(sizeof *ws);
    WorldCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.gen = gen_cb;
    cb.is_air = air_cb;                 /* sphere sparse-air predicate */
    if (ws == NULL || world_init(ws, 1u, &cb) != 0) { printf("init failed\n"); return 2; }

    printf("== test_sparse (0.5 M1 sparse-air storage) ==\n");

    /* The R=64 ball is centred at world (8,64,8) = chunk (0,4,0). Chunk (0,40,0)
     * is world-Y ~640, far above the ball -> wholly air. */
    {
        Chunk *air   = world_insert(ws, 0, 40, 0);
        Chunk *solid = world_insert(ws, 0, 4, 0);
        CHECK(air && air->voxels == NULL && (air->flags & CHUNK_UNIFORM),
              "exterior chunk inserts uniform-air (NULL voxels, no slab)");
        CHECK(air && vox_mat(chunk_vox(air, 0)) == MAT_AIR,
              "chunk_vox reads a uniform chunk as air");
        CHECK(solid && solid->voxels != NULL && !(solid->flags & CHUNK_UNIFORM),
              "interior chunk is realized (borrows a slab)");
        CHECK(solid && vox_mat(chunk_vox(solid, vox_index(8, 8, 8))) != MAT_AIR,
              "realized chunk holds solid content via chunk_vox");
    }

    /* Mesh: uniform-air -> 0 quads; realized solid -> >0. */
    {
        MeshBuffer mb;
        Chunk *air   = world_get(ws, 0, 40, 0);
        Chunk *solid = world_get(ws, 0, 4, 0);
        if (mesh_buffer_init(&mb, 65536u, 98304u) != 0) { printf("mb fail\n"); return 2; }
        CHECK(greedy_mesh(air, &mb) == 0, "uniform-air chunk meshes to 0 quads");
        CHECK(greedy_mesh(solid, &mb) > 0, "realized solid chunk meshes to >0 quads");
        mesh_buffer_free(&mb);
    }

    /* realize / set_uniform round-trip on the uniform chunk. */
    {
        Chunk *air = world_get(ws, 0, 40, 0);
        Voxel uw = air->uniform_word;
        uint32_t free_before = ws->slab_free_top;
        int i, all_uw = 1;
        CHECK(world_realize(ws, air) == 0 && air->voxels != NULL,
              "world_realize gives a uniform chunk a real block");
        for (i = 0; i < CHUNK_VOXELS; ++i)
            if (air->voxels[i] != uw) { all_uw = 0; break; }
        CHECK(all_uw, "realize expands the uniform word into every voxel (air, ambient temp)");
        world_set_uniform(ws, air, uw);
        CHECK(air->voxels == NULL && ws->slab_free_top == free_before,
              "set_uniform drops voxels and returns the slab to the pool");
    }

    /* Stream the full window around the ball: the working-set win + no overflow.
     * world_stream_update is budgeted, so loop until the window settles. */
    {
        uint32_t resident, realized;
        int k;
        for (k = 0; k < 600; ++k)
            world_stream_update(ws, 0.0f, 0.0f);
        resident = world_resident_count(ws);
        realized = (uint32_t)WORLD_SLAB_SLOTS - ws->slab_free_top;
        printf("  settled window: %u resident chunks, %u realized blocks "
               "(peak %u of %u slabs)\n",
               resident, realized, ws->slab_inuse_peak, (unsigned)WORLD_SLAB_SLOTS);
        CHECK(ws->slab_inuse_peak < (uint32_t)WORLD_SLAB_SLOTS,
              "slab sub-pool never exhausts over a full window stream (no overflow)");
        CHECK(resident > realized,
              "sparse win: resident chunks exceed realized blocks (air costs no slab)");
        CHECK((uint64_t)realized * 2u < resident,
              "the majority of the resident window is uniform-air (>50% slabs saved)");
        printf("  RAM: dense would reserve %u x 16 KiB = %.1f MiB of voxels; sparse "
               "touches %u blocks = %.1f MiB (record pool now ~%.0f KiB)\n",
               resident, resident * 16.0 / 1024.0,
               realized, realized * 16.0 / 1024.0,
               (double)WORLD_POOL_SLOTS * sizeof(Chunk) / 1024.0);
    }

    world_shutdown(ws);
    free(ws);
    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
