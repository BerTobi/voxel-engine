/* test_player.c - unit tests for the AABB physics module (player.c).
 *
 * Pure math, no engine deps: each `solid` predicate is a synthetic grid, so the
 * cases are deterministic. Covers landing, head-bonk, wall clamp, corner, the
 * anti-tunnel sub-step cap, water buoyancy/swim, and free-fall. */
#include <stdio.h>
#include <math.h>
#include "player.h"

static int g_fail = 0;
static void check(const char *name, int cond)
{
    if (cond) { printf("PASS: %s\n", name); }
    else      { printf("FAIL: %s\n", name); ++g_fail; }
}

/* ---- synthetic worlds (kind: 0 passable, 1 solid, 2 liquid) -------------- */
static int w_floor(void *c, int x, int y, int z){ (void)c;(void)x;(void)z; return y <= 0 ? 1 : 0; }
static int w_floor_ceil(void *c,int x,int y,int z){ (void)c;(void)x;(void)z; return (y<=0||y>=4)?1:0; }
static int w_floor_wallx(void *c,int x,int y,int z){ (void)c;(void)z; if(y<=0)return 1; return x>=2?1:0; }
static int w_floor_corner(void *c,int x,int y,int z){ if(y<=0)return 1; (void)c; return (x>=2||z>=2)?1:0; }
static int w_thinwall(void *c,int x,int y,int z){ (void)c;(void)y;(void)z; return x==3?1:0; }
static int w_water(void *c,int x,int y,int z){ (void)c;(void)x;(void)z; if(y<=0)return 1; return y<=6?2:0; }
static int w_empty(void *c,int x,int y,int z){ (void)c;(void)x;(void)y;(void)z; return 0; }

static Player at(float x, float y, float z)
{
    Player p;
    p.pos.x = x; p.pos.y = y; p.pos.z = z;
    p.vel.x = p.vel.y = p.vel.z = 0.0f;
    p.on_ground = 0; p.in_water = 0;
    return p;
}
static PlyVec v3(float x, float y, float z){ PlyVec v; v.x=x; v.y=y; v.z=z; return v; }

int main(void)
{
    PlyParams pp = player_defaults();
    PlyVec none = v3(0,0,0);
    const float DT = 1.0f / 30.0f;
    int i;

    /* (1) FALL + LAND: drop from y=5 onto the floor (block tops at y=1). Feet
     *     settle at ~1.0, on_ground, vel.y ~ 0. */
    {
        Player p = at(0.5f, 5.0f, 0.5f);
        for (i = 0; i < 90; ++i) player_step(&p, &pp, none, 0, 0, DT, w_floor, 0);
        check("land: feet rest at ~1.0", fabsf(p.pos.y - 1.0f) < 0.02f);
        check("land: on_ground set", p.on_ground == 1);
        check("land: vertical velocity ~0", fabsf(p.vel.y) < 0.5f);
    }

    /* (2) HEAD-BONK: floor + ceiling (solid y>=4). Jump from rest; the AABB top
     *     (feet+height) never penetrates y=4. */
    {
        Player p = at(0.5f, 1.0f, 0.5f);
        float max_head = 0.0f;
        for (i = 0; i < 90; ++i) {
            int jump = (i == 5);   /* one jump once grounded */
            player_step(&p, &pp, none, jump, 0, DT, w_floor_ceil, 0);
            if (p.pos.y + pp.height > max_head) max_head = p.pos.y + pp.height;
        }
        check("ceiling: head never passes y=4", max_head <= 4.0f + 0.02f);
    }

    /* (3) WALL: floor + solid x>=2. Walk +X; the AABB max (x+half) clamps to 2.0
     *     (x<=1.7), vel.x killed, no Z drift. */
    {
        Player p = at(0.0f, 1.0f, 0.5f);
        for (i = 0; i < 90; ++i) player_step(&p, &pp, v3(1,0,0), 0, 0, DT, w_floor_wallx, 0);
        check("wall: x clamps to 1.7 (max at 2.0)", p.pos.x <= 1.7f + 0.02f);
        check("wall: x velocity killed", fabsf(p.vel.x) < 0.01f);
        check("wall: no Z drift", fabsf(p.pos.z - 0.5f) < 0.02f);
    }

    /* (4) CORNER: floor + walls x>=2 AND z>=2. Diagonal wish stops in the corner
     *     without penetrating EITHER wall (per-axis resolution). */
    {
        Player p = at(0.0f, 1.0f, 0.0f);
        PlyVec diag = v3(0.7071f, 0.0f, 0.7071f);
        for (i = 0; i < 90; ++i) player_step(&p, &pp, diag, 0, 0, DT, w_floor_corner, 0);
        check("corner: x not penetrated", p.pos.x <= 1.7f + 0.02f);
        check("corner: z not penetrated", p.pos.z <= 1.7f + 0.02f);
        check("corner: position finite", p.pos.x == p.pos.x && p.pos.z == p.pos.z);
    }

    /* (5) ANTI-TUNNEL: a 1-voxel-thick wall at x==3 and a huge velocity that, in
     *     one un-substepped move, would jump clear past it. The sub-step cap must
     *     still stop the AABB at the wall (x+half <= 3.0). */
    {
        Player p = at(0.0f, 1.0f, 0.5f);
        p.vel.x = 600.0f;             /* ~20 voxels in one clamped 50ms frame */
        player_step(&p, &pp, none, 0, 0, 0.05f, w_thinwall, 0);
        check("anti-tunnel: stopped before the x=3 wall", p.pos.x + pp.half_xz <= 3.0f + 0.02f);
        check("anti-tunnel: did NOT pass through", p.pos.x < 3.0f);
    }

    /* (6) WATER: floor at y<=0, liquid y in [1,6]. Submerged sinking is capped at
     *     water_term (not the air terminal), in_water is set, and Space swims UP.
     *     Start submerged and sink toward the floor (so we stay in the water). */
    {
        Player p = at(0.5f, 5.0f, 0.5f);
        for (i = 0; i < 10; ++i) player_step(&p, &pp, none, 0, 0, DT, w_water, 0);
        check("water: in_water set", p.in_water == 1);
        check("water: sink capped at water_term",
              p.vel.y < 0.0f && p.vel.y >= -(pp.water_term) - 0.01f);
        for (i = 0; i < 40; ++i) player_step(&p, &pp, none, 0, 0, DT, w_water, 0); /* settle */
        player_step(&p, &pp, none, 1, 0, DT, w_water, 0);   /* Space = swim up */
        check("water: swim-up gives upward velocity", p.in_water == 1 && p.vel.y > 0.0f);
    }

    /* (7) FREE-FALL: empty world. Accelerates downward, never latches ground. */
    {
        Player p = at(0.5f, 50.0f, 0.5f);
        float y0;
        for (i = 0; i < 5; ++i) player_step(&p, &pp, none, 0, 0, DT, w_empty, 0);
        y0 = p.pos.y;
        for (i = 0; i < 20; ++i) player_step(&p, &pp, none, 0, 0, DT, w_empty, 0);
        check("freefall: never on_ground", p.on_ground == 0);
        check("freefall: keeps descending", p.pos.y < y0);
        check("freefall: negative velocity", p.vel.y < 0.0f);
    }

    if (g_fail == 0) printf("== ALL PASS ==\n");
    else             printf("== %d FAIL ==\n", g_fail);
    return g_fail ? 1 : 0;
}
