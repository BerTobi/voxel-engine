/* test_edit.c - unit tests for the voxel edit API (world_get_voxel /
 * world_edit_voxel), the 0.2 block break/place world hook.
 *
 * Headless WorldStore (gen only, no GL): prime a window, then verify reads,
 * edits, the MODIFIED+DIRTY_MESH flagging, the unloaded-coordinate no-op, and
 * boundary-neighbour dirtying. No GL, no platform. */
#include <stdio.h>
#include <stdlib.h>
#include "world.h"
#include "worldgen.h"
#include "chunk.h"
#include "voxel.h"
#include "material.h"

static int g_fail = 0;
static void check(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); ++g_fail; }
}

static void gen_cb(Chunk *c, int cx, int cy, int cz, uint64_t s, void *u)
{
    (void)u;
    worldgen_fill_chunk(c, cx, cy, cz, s);
}

int main(void)
{
    WorldStore *ws = (WorldStore *)malloc(sizeof(WorldStore));
    WorldCallbacks cb;
    cb.gen = gen_cb; cb.mesh_upload = NULL; cb.slot_free = NULL; cb.user = NULL;
    cb.is_air = NULL;   /* 0.5 M1: dense (no sparse-air collapse) for this test */
    if (ws == NULL || world_init(ws, 777u, &cb) != 0) {
        printf("init failed\n");
        return 1;
    }
    /* Resident window around the origin; the drain leaves every chunk clean. */
    world_prime(ws, 0.0f, 24.0f, 0.0f);

    /* 1) Read + edit a resident INTERIOR voxel: world (8,20,8) -> chunk (0,1,0),
     *    local (8,4,8). Read-back matches, chunk flagged MODIFIED + DIRTY_MESH. */
    {
        Chunk *c = world_get(ws, 0, 1, 0);
        Voxel stone = 0;
        int ok;
        Voxel after;
        vox_set_mat(&stone, MAT_STONE);
        vox_set_fill(&stone, 15);
        check("chunk (0,1,0) resident + clean after prime",
              c != NULL && !(c->flags & CHUNK_DIRTY_MESH));
        ok = world_edit_voxel(ws, 8, 20, 8, stone);
        after = world_get_voxel(ws, 8, 20, 8);
        check("edit resident voxel returns 1", ok == 1);
        check("read-back is the edited material", vox_mat(after) == MAT_STONE);
        check("chunk now CHUNK_MODIFIED",   c && (c->flags & CHUNK_MODIFIED));
        check("chunk now CHUNK_DIRTY_MESH", c && (c->flags & CHUNK_DIRTY_MESH));
    }

    /* 2) Editing an UNLOADED voxel (far outside the window) is a no-op (returns
     *    0); reading it returns air. */
    {
        int ok = world_edit_voxel(ws, 100000, 20, 100000, (Voxel)0);
        check("edit unloaded voxel returns 0", ok == 0);
        check("unloaded read is air",
              vox_mat(world_get_voxel(ws, 100000, 20, 100000)) == (uint8_t)MAT_AIR);
    }

    /* 3) A boundary-plane edit (local x==0) dirties the abutting neighbour chunk
     *    so the shared seam re-meshes. world (0,20,8) -> chunk (0,1,0) local
     *    (0,4,8); the -X neighbour is chunk (-1,1,0). */
    {
        Chunk *nb = world_get(ws, -1, 1, 0);
        if (nb) nb->flags &= (uint8_t)~CHUNK_DIRTY_MESH;   /* start clean */
        check("-x neighbour resident", nb != NULL);
        world_edit_voxel(ws, 0, 20, 8, (Voxel)0);
        check("boundary edit dirties the -x neighbour",
              nb && (nb->flags & CHUNK_DIRTY_MESH));
    }

    world_shutdown(ws);
    free(ws);
    if (g_fail == 0) printf("== ALL PASS ==\n");
    else             printf("== %d FAIL ==\n", g_fail);
    return g_fail ? 1 : 0;
}
