/* test_raycast.c - unit tests for the voxel DDA (raycast.c).
 *
 * Pure math, no engine deps: the `solid` predicate is a synthetic grid, so each
 * case is fully deterministic. Covers floor/wall hits, a clean miss, negative
 * coordinates, origin-inside-solid, and the place-cell (the empty voxel before
 * the hit) used for block placement. */
#include <stdio.h>
#include "raycast.h"

static int g_fail = 0;

static void check(const char *name, int cond)
{
    if (cond) { printf("PASS: %s\n", name); }
    else      { printf("FAIL: %s\n", name); ++g_fail; }
}

/* solid iff y <= 0  (a ground plane; everything at/below y=0 is rock). */
static int floor_world(void *ctx, int x, int y, int z) { (void)ctx;(void)x;(void)z; return y <= 0; }
/* solid iff x >= 5  (a wall to the +X). */
static int wall_world(void *ctx, int x, int y, int z)  { (void)ctx;(void)y;(void)z; return x >= 5; }
/* solid iff exactly the voxel (2,3,4). */
static int block_world(void *ctx, int x, int y, int z) { (void)ctx; return x==2 && y==3 && z==4; }
/* nothing is ever solid. */
static int empty_world(void *ctx, int x, int y, int z) { (void)ctx;(void)x;(void)y;(void)z; return 0; }

int main(void)
{
    /* 1) Straight down onto the floor: from (0.5,5.5,0.5) along -Y. The ground
     *    top voxel is y=0; the place cell is the empty voxel just above (y=1). */
    {
        RayHit r = raycast_voxel(0.5f, 5.5f, 0.5f, 0.0f, -1.0f, 0.0f, 16.0f, floor_world, 0);
        check("floor: hit", r.hit == 1);
        check("floor: hit voxel y=0", r.hy == 0 && r.hx == 0 && r.hz == 0);
        check("floor: place voxel y=1 (cell before hit)", r.py == 1 && r.px == 0 && r.pz == 0);
    }

    /* 2) Toward +X into a wall at x>=5: from (0.5,0.5,0.5) along +X. First solid
     *    voxel is x=5; place cell is x=4. */
    {
        RayHit r = raycast_voxel(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 16.0f, wall_world, 0);
        check("wall: hit", r.hit == 1);
        check("wall: hit voxel x=5", r.hx == 5);
        check("wall: place voxel x=4", r.px == 4);
    }

    /* 3) Reach too short: the wall is 4.5 units away but max_dist is 2. Miss. */
    {
        RayHit r = raycast_voxel(0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 2.0f, wall_world, 0);
        check("short reach: miss", r.hit == 0);
    }

    /* 4) Empty world: never hits regardless of reach. */
    {
        RayHit r = raycast_voxel(0.5f, 0.5f, 0.5f, 0.3f, 0.7f, 0.1f, 100.0f, empty_world, 0);
        check("empty world: miss", r.hit == 0);
    }

    /* 5) Single block at (2,3,4): aim straight at it from (2.5,3.5,9.0) along -Z.
     *    Hit (2,3,4); place the cell in front (z=5). */
    {
        RayHit r = raycast_voxel(2.5f, 3.5f, 9.0f, 0.0f, 0.0f, -1.0f, 16.0f, block_world, 0);
        check("block: hit (2,3,4)", r.hit == 1 && r.hx==2 && r.hy==3 && r.hz==4);
        check("block: place z=5", r.px==2 && r.py==3 && r.pz==5);
    }

    /* 6) NEGATIVE coords: floor at y<=0, ray from (-3.5, 4.0, -3.5) along -Y.
     *    Hit y=0 at the correct (floored) x/z; place y=1. */
    {
        RayHit r = raycast_voxel(-3.5f, 4.0f, -3.5f, 0.0f, -1.0f, 0.0f, 16.0f, floor_world, 0);
        check("neg: hit", r.hit == 1);
        check("neg: hit x=-4 y=0 z=-4", r.hx==-4 && r.hy==0 && r.hz==-4);
        check("neg: place y=1", r.py==1);
    }

    /* 7) Origin already inside a solid (standing in rock): immediate hit, place
     *    cell == hit cell. */
    {
        RayHit r = raycast_voxel(3.5f, -2.5f, 3.5f, 0.0f, 1.0f, 0.0f, 16.0f, floor_world, 0);
        check("inside-solid: immediate hit", r.hit == 1);
        check("inside-solid: place==hit", r.px==r.hx && r.py==r.hy && r.pz==r.hz);
    }

    /* 8) Diagonal ray onto the floor still lands on a y=0 voxel. */
    {
        RayHit r = raycast_voxel(0.5f, 6.0f, 0.5f, 0.4f, -1.0f, 0.2f, 32.0f, floor_world, 0);
        check("diagonal: hit floor y=0", r.hit == 1 && r.hy == 0);
        check("diagonal: place is one above hit", r.py == r.hy + 1 && r.px == r.hx && r.pz == r.hz);
    }

    if (g_fail == 0) printf("== ALL PASS ==\n");
    else             printf("== %d FAIL ==\n", g_fail);
    return g_fail ? 1 : 0;
}
