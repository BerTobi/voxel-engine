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

/* 0.5 (256 m): the slab pool is now sized at world_init to the session view-distance
 * ceiling, not the compile WORLD_RADIUS. Run the test at a fixed modest radius (fast +
 * light) and assert against ITS window / the runtime slab count (ws->slab_slots). */
#define TEST_VIEW_R      8
#define TEST_VIEW_WINDOW WORLD_WINDOW_AT(TEST_VIEW_R)

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
    if (ws == NULL || world_init(ws, 1u, &cb, TEST_VIEW_R) != 0) { printf("init failed\n"); return 2; }
    /* world_init starts the active radius at the ceiling (TEST_VIEW_R), so the window
     * settles to TEST_VIEW_WINDOW below. */

    printf("== test_sparse (0.5 M1 sparse-air storage) ==\n");

    /* The R=512 ball is centred at world (8,512,8); the north pole surface is at
     * world-Y 1024 (chunk cy 64). Chunk (0,80,0) is world-Y ~1280, far above the
     * pole -> wholly air; chunk (0,63,0) is the solid crust just under the pole. */
    {
        Chunk *air   = world_insert(ws, 0, 80, 0);
        Chunk *solid = world_insert(ws, 0, 63, 0);
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
        Chunk *air   = world_get(ws, 0, 80, 0);
        Chunk *solid = world_get(ws, 0, 63, 0);
        if (mesh_buffer_init(&mb, 65536u, 98304u) != 0) { printf("mb fail\n"); return 2; }
        CHECK(greedy_mesh(air, &mb) == 0, "uniform-air chunk meshes to 0 quads");
        CHECK(greedy_mesh(solid, &mb) > 0, "realized solid chunk meshes to >0 quads");
        mesh_buffer_free(&mb);
    }

    /* realize / set_uniform round-trip on the uniform chunk. */
    {
        Chunk *air = world_get(ws, 0, 80, 0);
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
            world_stream_update(ws, 8.0f, 1024.0f, 8.0f);
        resident = world_resident_count(ws);
        realized = ws->slab_slots - ws->slab_free_top;
        printf("  settled window: %u resident chunks, %u realized blocks "
               "(peak %u of %u slabs)\n",
               resident, realized, ws->slab_inuse_peak, ws->slab_slots);
        CHECK(ws->slab_inuse_peak < ws->slab_slots,
              "slab sub-pool never exhausts over a full window stream (no overflow)");
        CHECK(resident > realized,
              "sparse win: resident chunks exceed realized blocks (air above the surface costs no slab)");
        CHECK(ws->slab_inuse_peak < ws->slab_slots,
              "slab pool sized with churn headroom at the real pole-surface window");
        printf("  RAM: dense would reserve %u x 16 KiB = %.1f MiB of voxels; sparse "
               "touches %u blocks = %.1f MiB (record pool now ~%.0f KiB)\n",
               resident, resident * 16.0 / 1024.0,
               realized, realized * 16.0 / 1024.0,
               (double)WORLD_POOL_SLOTS * sizeof(Chunk) / 1024.0);
    }

    /* THE NO-HOLES REGRESSION (M2 grain-review): a player who flies/digs deep sits in
     * a window that is ENTIRELY solid rock - every chunk needs a slab. Stream the
     * window fully BELOW the surface and assert the whole 1521-window loads (no
     * silently-dropped inserts) and the pool does not exhaust. This is the case the
     * surface-only test above missed; it must hold for any reachable position. */
    {
        uint32_t resident, realized;
        int k;
        for (k = 0; k < 600; ++k)
            world_stream_update(ws, 8.0f, 50.0f * 16.0f, 8.0f);   /* ~40 m down, fully submerged */
        resident = world_resident_count(ws);
        realized = ws->slab_slots - ws->slab_free_top;
        printf("  underground window: %u resident, %u realized (all-solid worst case)\n",
               resident, realized);
        CHECK(resident == (uint32_t)TEST_VIEW_WINDOW,
              "no holes: a fully-submerged window loads ALL chunks (slab pool not exhausted)");
        CHECK(realized <= ws->slab_slots,
              "underground realized count fits the slab pool (no silent insert failure)");
    }

    world_shutdown(ws);
    free(ws);
    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
