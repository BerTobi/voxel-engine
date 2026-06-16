/* player.c - AABB collision + gravity + walk/jump/swim (see player.h).
 *
 * Per frame: derive horizontal velocity from the wish direction (snappy on the
 * ground, eased in air), apply gravity (capped at terminal, or a slow sink in
 * water), then MOVE-AND-RESOLVE the displacement PER AXIS (Y, then X, then Z),
 * sub-stepped so no single advance exceeds max_substep voxels - that sub-step
 * cap is the anti-tunnelling guard (a fast move is split into <0.5-voxel hops,
 * each checked against the world). Collision blocks only on solidity kind 1
 * (opaque); liquid (kind 2) is passable and just sets buoyancy. Float-only.
 */
#include "player.h"
#include <math.h>

/* floor() toward -inf as an int (plain (int) truncates toward zero - wrong for
 * negative world coordinates). Identical to raycast.c's helper. */
static int ifloor(float f)
{
    int i = (int)f;
    return (f < (float)i) ? i - 1 : i;
}

/* Does the player's AABB overlap any cell whose solidity == want_kind? The eps
 * on the max edges keeps a box flush against an integer plane (e.g. feet exactly
 * on y=N, standing on the block at y=N-1) from spuriously including the next
 * cell - without it a resting player reads the floor as a side overlap and
 * sticks/jitters. */
static int aabb_overlaps(const Player *p, const PlyParams *pp,
                         PlySolidFn solid, void *ctx, int want_kind)
{
    const float eps = 1e-4f;
    int x0 = ifloor(p->pos.x - pp->half_xz);
    int x1 = ifloor(p->pos.x + pp->half_xz - eps);
    int y0 = ifloor(p->pos.y);
    int y1 = ifloor(p->pos.y + pp->height - eps);
    int z0 = ifloor(p->pos.z - pp->half_xz);
    int z1 = ifloor(p->pos.z + pp->half_xz - eps);
    int x, y, z;
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            for (z = z0; z <= z1; ++z)
                if (solid(ctx, x, y, z) == want_kind)
                    return 1;
    return 0;
}

PlyParams player_defaults(void)
{
    PlyParams pp;
    pp.half_xz        = 0.3f;
    pp.height         = 1.8f;
    pp.eye            = 1.62f;
    pp.gravity        = 28.0f;
    pp.term_fall      = 56.0f;
    pp.walk_speed     = 4.3f;
    pp.jump_vel       = 8.4f;
    pp.air_control    = 0.35f;
    pp.water_xy_scale = 0.5f;
    pp.water_term     = 3.0f;
    pp.swim_vel       = 4.0f;
    pp.max_substep    = 0.45f;
    return pp;
}

void player_step(Player *p, const PlyParams *pp, PlyVec wish,
                 int want_jump, int want_down, float dt,
                 PlySolidFn solid, void *ctx)
{
    int in_water, was_ground, s, n;
    PlyVec d, st;
    float tx, tz, term, ax, ay, az, md;

    /* Clamp dt so the per-frame displacement (and thus the sub-step count) stays
     * bounded at very low frame rates - a tighter cap than the render loop's
     * 250 ms guard. Physics goes slow-motion below ~20 FPS rather than tunnel. */
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0f)  dt = 0.0f;

    was_ground = p->on_ground;

    /* Buoyancy state sampled BEFORE moving (speed scaling + terminal sink). */
    in_water = aabb_overlaps(p, pp, solid, ctx, 2);

    /* Horizontal velocity: snap to intent on the ground, ease toward it in air. */
    tx = wish.x * pp->walk_speed;
    tz = wish.z * pp->walk_speed;
    if (was_ground) {
        p->vel.x = tx;
        p->vel.z = tz;
    } else {
        p->vel.x += (tx - p->vel.x) * pp->air_control;
        p->vel.z += (tz - p->vel.z) * pp->air_control;
    }
    if (in_water) {
        p->vel.x *= pp->water_xy_scale;
        p->vel.z *= pp->water_xy_scale;
    }

    /* Gravity, capped at terminal (a gentler terminal while submerged). */
    p->vel.y -= pp->gravity * dt;
    term = in_water ? pp->water_term : pp->term_fall;
    if (p->vel.y < -term) p->vel.y = -term;

    /* Jump (ground) / swim-up (water); crouch is swim-down in water. */
    if (want_jump) {
        if (in_water)            p->vel.y = pp->swim_vel;
        else if (was_ground)     p->vel.y = pp->jump_vel;
    }
    if (want_down && in_water)   p->vel.y = -pp->swim_vel;

    /* Integrate with per-axis move-and-resolve, sub-stepped to <max_substep. */
    d.x = p->vel.x * dt;
    d.y = p->vel.y * dt;
    d.z = p->vel.z * dt;
    ax = fabsf(d.x); ay = fabsf(d.y); az = fabsf(d.z);
    md = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
    n = (int)ceilf(md / pp->max_substep);
    if (n < 1) n = 1;
    st.x = d.x / (float)n;
    st.y = d.y / (float)n;
    st.z = d.z / (float)n;

    p->on_ground = 0;
    for (s = 0; s < n; ++s) {
        /* Y first: resolve vertical so landing/headbonk set the ground flag. */
        p->pos.y += st.y;
        if (aabb_overlaps(p, pp, solid, ctx, 1)) {
            p->pos.y -= st.y;
            if (st.y < 0.0f) p->on_ground = 1;   /* blocked while descending */
            p->vel.y = 0.0f;                      /* land or head-bonk        */
            st.y = 0.0f;
        }
        /* X */
        p->pos.x += st.x;
        if (aabb_overlaps(p, pp, solid, ctx, 1)) {
            p->pos.x -= st.x;
            p->vel.x = 0.0f;
            st.x = 0.0f;
        }
        /* Z (independent of X so the player slides along a wall in a corner). */
        p->pos.z += st.z;
        if (aabb_overlaps(p, pp, solid, ctx, 1)) {
            p->pos.z -= st.z;
            p->vel.z = 0.0f;
            st.z = 0.0f;
        }
    }

    /* Caller-visible buoyancy flag after the move. */
    p->in_water = aabb_overlaps(p, pp, solid, ctx, 2);
}
