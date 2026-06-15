/* raycast.h - voxel ray cast (Amanatides & Woo DDA) for block break/place.
 *
 * Pure integer-grid traversal decoupled from the world store: the caller supplies
 * a `solid` predicate, so the same routine drives the engine (sampling the
 * streamed world) and the unit tests (sampling a synthetic grid). Float-light and
 * allocation-free; the hot path is the per-frame "what block am I aiming at".
 */
#ifndef RAYCAST_H
#define RAYCAST_H

/* Returns non-zero if the voxel (x,y,z) BLOCKS the ray (e.g. an opaque solid).
 * `ctx` is the opaque user pointer passed to raycast_voxel. */
typedef int (*RayVoxelFn)(void *ctx, int x, int y, int z);

typedef struct {
    int hit;            /* 1 if a blocking voxel was found within max_dist, else 0 */
    int hx, hy, hz;     /* the blocking voxel hit (the BREAK target)               */
    int px, py, pz;     /* the last empty voxel before it (the PLACE target);      *
                         * == the hit voxel if the ray started already inside one  */
} RayHit;

/* Cast from float world position (ox,oy,oz) along (dx,dy,dz) up to max_dist world
 * units, stepping voxel-by-voxel. The direction is normalized internally, so
 * max_dist is always in world units (voxels) regardless of the input length; a
 * zero-length direction just tests the origin voxel. Stops at (and returns) the
 * first voxel for which solid(ctx,...) is non-zero. */
RayHit raycast_voxel(float ox, float oy, float oz,
                     float dx, float dy, float dz,
                     float max_dist, RayVoxelFn solid, void *ctx);

#endif /* RAYCAST_H */
