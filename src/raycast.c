/* raycast.c - Amanatides & Woo voxel DDA (see raycast.h).
 *
 * "A Fast Voxel Traversal Algorithm for Ray Tracing" (Amanatides & Woo, 1987):
 * walk the integer voxel grid one cell at a time, always advancing along the axis
 * whose next cell-boundary crossing is nearest in ray parameter t. Exact (no
 * skipped or double-visited cells) and branch-light. We track the previous cell
 * so a hit also yields the empty neighbour to place a block into.
 */
#include "raycast.h"
#include <math.h>

/* floor() toward -inf as an int (plain (int) truncates toward zero, which is
 * wrong for negative world coordinates). */
static int ifloor(float f)
{
    int i = (int)f;
    return (f < (float)i) ? i - 1 : i;
}

RayHit raycast_voxel(float ox, float oy, float oz,
                     float dx, float dy, float dz,
                     float max_dist, RayVoxelFn solid, void *ctx)
{
    const float INF = 1e30f;
    RayHit r;
    int x = ifloor(ox), y = ifloor(oy), z = ifloor(oz);
    int px = x, py = y, pz = z;
    int stepx, stepy, stepz;
    float tMaxX, tMaxY, tMaxZ, tDeltaX, tDeltaY, tDeltaZ;
    float len;

    r.hit = 0;
    r.hx = r.px = x;
    r.hy = r.py = y;
    r.hz = r.pz = z;

    /* Normalize so max_dist is in world units regardless of input length. */
    len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len > 0.0f) { dx /= len; dy /= len; dz /= len; }

    /* You may already be standing inside a solid; report it immediately (the
     * place target is then the same cell - the caller can choose to ignore it). */
    if (solid(ctx, x, y, z)) {
        r.hit = 1;
        return r;
    }

    stepx = (dx > 0.0f) ? 1 : ((dx < 0.0f) ? -1 : 0);
    stepy = (dy > 0.0f) ? 1 : ((dy < 0.0f) ? -1 : 0);
    stepz = (dz > 0.0f) ? 1 : ((dz < 0.0f) ? -1 : 0);

    /* t to the first voxel boundary on each axis, and t to cross one whole voxel.
     * A zero direction component never advances that axis (tMax/tDelta = INF). */
    if (stepx != 0) {
        float nb = (stepx > 0) ? (float)(x + 1) : (float)x;
        tMaxX = (nb - ox) / dx; tDeltaX = 1.0f / (dx > 0 ? dx : -dx);
    } else { tMaxX = INF; tDeltaX = INF; }
    if (stepy != 0) {
        float nb = (stepy > 0) ? (float)(y + 1) : (float)y;
        tMaxY = (nb - oy) / dy; tDeltaY = 1.0f / (dy > 0 ? dy : -dy);
    } else { tMaxY = INF; tDeltaY = INF; }
    if (stepz != 0) {
        float nb = (stepz > 0) ? (float)(z + 1) : (float)z;
        tMaxZ = (nb - oz) / dz; tDeltaZ = 1.0f / (dz > 0 ? dz : -dz);
    } else { tMaxZ = INF; tDeltaZ = INF; }

    for (;;) {
        float t;
        px = x; py = y; pz = z;            /* remember the empty cell we leave */
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ)      { x += stepx; t = tMaxX; tMaxX += tDeltaX; }
        else if (tMaxY <= tMaxZ)                   { y += stepy; t = tMaxY; tMaxY += tDeltaY; }
        else                                       { z += stepz; t = tMaxZ; tMaxZ += tDeltaZ; }

        if (t > max_dist)
            break;                          /* out of reach: no hit */

        if (solid(ctx, x, y, z)) {
            r.hit = 1;
            r.hx = x;  r.hy = y;  r.hz = z;
            r.px = px; r.py = py; r.pz = pz;
            return r;
        }
    }
    return r;                               /* r.hit == 0 */
}
