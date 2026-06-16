/* player.c - RADIAL gravity + sphere collision + walk/jump/swim (see player.h).
 *
 * Per frame: build the local radial frame (up = away from the planet center of
 * mass), decompose velocity into radial + tangential, drive the tangential part
 * from the wish direction (snappy on the ground, eased in air), apply RADIAL
 * gravity (capped at terminal), then integrate sub-stepped (anti-tunnel) and
 * DEPENETRATE a collision SPHERE out of solid voxels. A sphere is orientation-
 * free, so it works anywhere on the planet without reorienting a box (a box would
 * lie sideways at the equator). Collision blocks only on solidity kind 1 (opaque);
 * liquid (kind 2) is passable and just sets buoyancy. Float-only - it never feeds
 * the deterministic integer sim. A far-away center ~= flat -Y gravity again.
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

/* Small float vector helpers (player-local; the engine's Vec3 lives in main.c). */
static PlyVec pv_sub(PlyVec a, PlyVec b)  { PlyVec r; r.x=a.x-b.x; r.y=a.y-b.y; r.z=a.z-b.z; return r; }
static PlyVec pv_add(PlyVec a, PlyVec b)  { PlyVec r; r.x=a.x+b.x; r.y=a.y+b.y; r.z=a.z+b.z; return r; }
static PlyVec pv_scale(PlyVec a, float s) { PlyVec r; r.x=a.x*s; r.y=a.y*s; r.z=a.z*s; return r; }
static float  pv_dot(PlyVec a, PlyVec b)  { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float  pv_len(PlyVec a)            { return sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); }

/* Closest point on the unit voxel cube [vx,vx+1]^3 to p (per-axis clamp). */
static PlyVec cube_closest(PlyVec p, int vx, int vy, int vz)
{
    PlyVec c = p;
    if (c.x < (float)vx) c.x = (float)vx; else if (c.x > (float)vx + 1.0f) c.x = (float)vx + 1.0f;
    if (c.y < (float)vy) c.y = (float)vy; else if (c.y > (float)vy + 1.0f) c.y = (float)vy + 1.0f;
    if (c.z < (float)vz) c.z = (float)vz; else if (c.z > (float)vz + 1.0f) c.z = (float)vz + 1.0f;
    return c;
}

/* Does the player's collision SPHERE (radius r_p about pos) overlap any voxel of
 * solidity kind `want_kind`? Orientation-free, so the same test works anywhere on
 * the planet without a local frame (the whole reason a sphere was chosen). */
static int sphere_overlaps(const Player *p, const PlyParams *pp,
                           PlySolidFn solid, void *ctx, int want_kind)
{
    float r = pp->r_p, r2 = r * r;
    int x0 = ifloor(p->pos.x - r), x1 = ifloor(p->pos.x + r);
    int y0 = ifloor(p->pos.y - r), y1 = ifloor(p->pos.y + r);
    int z0 = ifloor(p->pos.z - r), z1 = ifloor(p->pos.z + r);
    int x, y, z;
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            for (z = z0; z <= z1; ++z) {
                PlyVec d;
                if (solid(ctx, x, y, z) != want_kind) continue;
                d = pv_sub(p->pos, cube_closest(p->pos, x, y, z));
                if (pv_dot(d, d) < r2) return 1;
            }
    return 0;
}

/* Deepest-penetrating SOLID (kind 1) voxel overlapping the sphere -> the minimal
 * push-out vector (pos += *push clears it). Returns 1 if any overlap. Resolving
 * the single deepest contact per call, iterated by the caller, is more stable on
 * a stair-stepped voxel sphere than summing all pushes (which over-shoots at
 * edges/corners). */
static int sphere_resolve(const Player *p, const PlyParams *pp,
                          PlySolidFn solid, void *ctx, PlyVec *push)
{
    float r = pp->r_p, best = 0.0f;
    int x0 = ifloor(p->pos.x - r), x1 = ifloor(p->pos.x + r);
    int y0 = ifloor(p->pos.y - r), y1 = ifloor(p->pos.y + r);
    int z0 = ifloor(p->pos.z - r), z1 = ifloor(p->pos.z + r);
    int x, y, z;
    push->x = push->y = push->z = 0.0f;
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            for (z = z0; z <= z1; ++z) {
                PlyVec d;
                float d2, dl, pen;
                if (solid(ctx, x, y, z) != 1) continue;
                d  = pv_sub(p->pos, cube_closest(p->pos, x, y, z));
                d2 = pv_dot(d, d);
                if (d2 >= r * r) continue;            /* no overlap */
                dl  = sqrtf(d2);
                pen = r - dl;
                if (pen > best) {
                    best = pen;
                    if (dl > 1e-6f) *push = pv_scale(d, pen / dl);
                    else { push->x = 0.0f; push->y = pen; push->z = 0.0f; }
                }
            }
    return best > 0.0f;
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
    pp.r_p            = 0.45f;   /* collision sphere radius                  */
    pp.center_x       = 0.0f;    /* planet center; the CALLER overrides this */
    pp.center_y       = 0.0f;
    pp.center_z       = 0.0f;
    return pp;
}

void player_step(Player *p, const PlyParams *pp, PlyVec wish,
                 int want_jump, int want_down, float dt,
                 PlySolidFn solid, void *ctx)
{
    PlyVec center, up, vtan, d, stepd, w;
    float vrad, term, dist, md, wl;
    int was_ground, in_water, n, s, it;

    /* Clamp dt so the per-frame displacement (and thus the sub-step count) stays
     * bounded at very low frame rates. Physics goes slow-mo below ~20 FPS. */
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0f)  dt = 0.0f;
    was_ground = p->on_ground;

    /* Local radial frame: up points AWAY from the planet center of mass; "down"
     * (gravity) is -up. A far-away center makes up ~= +Y (locally flat). */
    center.x = pp->center_x; center.y = pp->center_y; center.z = pp->center_z;
    up   = pv_sub(p->pos, center);
    dist = pv_len(up);
    if (dist > 1e-4f) up = pv_scale(up, 1.0f / dist);
    else { up.x = 0.0f; up.y = 1.0f; up.z = 0.0f; }   /* degenerate: at the center */

    /* Buoyancy sampled BEFORE moving. */
    in_water = sphere_overlaps(p, pp, solid, ctx, 2);

    /* Decompose velocity into RADIAL (along up) + TANGENTIAL (surface plane). */
    vrad = pv_dot(p->vel, up);
    vtan = pv_sub(p->vel, pv_scale(up, vrad));

    /* Tangential velocity from the wish dir (re-projected into the tangent plane):
     * snap to intent on the ground, ease toward it in air. */
    w  = pv_sub(wish, pv_scale(up, pv_dot(wish, up)));
    wl = pv_len(w);
    if (wl > 1e-4f) w = pv_scale(w, 1.0f / wl);
    {
        PlyVec target = pv_scale(w, pp->walk_speed);
        if (was_ground) vtan = target;
        else            vtan = pv_add(vtan, pv_scale(pv_sub(target, vtan), pp->air_control));
        if (in_water)   vtan = pv_scale(vtan, pp->water_xy_scale);
    }

    /* Radial gravity, capped at terminal (gentler while submerged). */
    vrad -= pp->gravity * dt;
    term  = in_water ? pp->water_term : pp->term_fall;
    if (vrad < -term) vrad = -term;

    /* Jump = radially OUT (ground) / swim-up (water); crouch = swim-down. */
    if (want_jump) {
        if (in_water)            vrad = pp->swim_vel;
        else if (was_ground)     vrad = pp->jump_vel;
    }
    if (want_down && in_water)   vrad = -pp->swim_vel;

    /* Recompose, then integrate sub-stepped along the move magnitude (anti-tunnel). */
    p->vel = pv_add(vtan, pv_scale(up, vrad));
    d  = pv_scale(p->vel, dt);
    md = pv_len(d);
    n  = (int)ceilf(md / pp->max_substep);
    if (n < 1) n = 1;
    stepd = pv_scale(d, 1.0f / (float)n);

    p->on_ground = 0;
    for (s = 0; s < n; ++s) {
        p->pos = pv_add(p->pos, stepd);
        /* Sphere depenetration: shove out of the deepest solid, a few iterations
         * (handles edges/corners of the stair-stepped voxel surface). */
        for (it = 0; it < 4; ++it) {
            PlyVec push, nrm;
            float pl, vn;
            if (!sphere_resolve(p, pp, solid, ctx, &push)) break;
            p->pos = pv_add(p->pos, push);
            pl = pv_len(push);
            if (pl <= 1e-6f) break;
            nrm = pv_scale(push, 1.0f / pl);
            vn  = pv_dot(p->vel, nrm);
            if (vn < 0.0f) p->vel = pv_sub(p->vel, pv_scale(nrm, vn)); /* kill into-surface vel */
            if (pv_dot(nrm, up) > 0.5f) p->on_ground = 1;              /* pushed up => standing */
        }
    }

    /* Caller-visible buoyancy flag after the move. */
    p->in_water = sphere_overlaps(p, pp, solid, ctx, 2);
}
