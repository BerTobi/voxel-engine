/* test_worldgen.c - 0.5 radial terrain relief (worldgen.c). GL-free, no sockets.
 *
 * Asserts the relief contract worldgen_fill_chunk leans on:
 *   (1) worldgen_radial_offset is BOUNDED to [-WG_RELIEF_AMP, +WG_RELIEF_AMP],
 *   (2) DETERMINISTIC (same offset-from-centre -> same displacement, every call),
 *   (3) POLE-FLATTENED (0 on the Y axis; small near it) so the spawn pole + forge
 *       sit on predictable solid crust,
 *   (4) NON-TRIVIAL (the planet actually has hills + basins, not a constant radius),
 *   (5) CONTINUOUS (adjacent directions differ by a bounded amount - no cliffs that
 *       would tear the mesh / trap water), and
 *   (6) the generated planet is sane: deep core is STONE, far space is AIR, the pole
 *       column is solid crust (forge intact), and the surface radius VARIES.
 */
#include <stdio.h>
#include <stdlib.h>
#include "worldgen.h"
#include "chunk.h"
#include "voxel.h"
#include "material.h"

static int g_fail = 0;
static void check(const char *n, int ok) { if (ok) printf("PASS: %s\n", n); else { printf("FAIL: %s\n", n); ++g_fail; } }

int main(void)
{
    int i, dx, dy, dz;
    int seen_pos = 0, seen_neg = 0, bounded = 1, deterministic = 1, continuous = 1;
    const int R = WG_PLANET_R;

    printf("=== worldgen radial-relief tests ===\n");

    /* (1)+(2)+(4)+(5): sweep a grid of directions around the sphere. */
    for (dx = -R; dx <= R; dx += 23)
        for (dy = -R; dy <= R; dy += 23)
            for (dz = -R; dz <= R; dz += 23) {
                int off = worldgen_radial_offset(dx, dy, dz);
                if (off < -WG_RELIEF_AMP || off > WG_RELIEF_AMP) bounded = 0;
                if (worldgen_radial_offset(dx, dy, dz) != off) deterministic = 0;
                if (off > 0) seen_pos = 1;
                if (off < 0) seen_neg = 1;
            }
    check("radial offset stays within [-AMP, +AMP]", bounded);
    check("radial offset is deterministic (pure function)", deterministic);
    check("relief is non-trivial: surface bulges OUT somewhere", seen_pos);
    check("relief is non-trivial: surface dips IN (basins) somewhere", seen_neg);

    /* (3) pole-flatten: exactly on the +Y axis the displacement is 0; near it small. */
    check("relief is 0 on the +Y pole axis (flat forge/spawn crust)",
          worldgen_radial_offset(0, R, 0) == 0);
    check("relief is 0 on the -Y pole axis", worldgen_radial_offset(0, -R, 0) == 0);
    {
        int near = worldgen_radial_offset(4, R, 4);   /* 4 vox off the axis, deep in the flat zone */
        check("relief stays small just off the pole axis", near > -4 && near < 4);
    }

    /* (5) continuity: stepping one voxel along the surface changes the offset by a
     * bounded amount (a smooth field - no terrain cliffs). Sample a mid-latitude
     * arc (away from the flat pole) and bound the per-step delta. */
    {
        int prev = worldgen_radial_offset(R, 8, 0), max_step = 0;
        for (i = 1; i <= 200; ++i) {
            int cur = worldgen_radial_offset(R, 8, i);  /* walk in +Z across the side */
            int d = cur - prev; if (d < 0) d = -d;
            if (d > max_step) max_step = d;
            prev = cur;
        }
        if (max_step > 4) continuous = 0;               /* <= 4 vox per voxel-step */
        check("relief is continuous (no cliff > 4 vox between adjacent columns)", continuous);
    }

    /* (6) generated-planet sanity via worldgen_fill_chunk. */
    {
        Chunk *c = malloc(sizeof *c);
        if (c == NULL) { check("alloc test chunk", 0); printf("=== %d failure(s) ===\n", g_fail); return g_fail ? 1 : 0; }
        c->voxels = malloc((size_t)CHUNK_VOXELS * sizeof(Voxel));
        c->slab_idx = -1;
        if (c->voxels == NULL) { check("alloc test chunk voxels", 0); free(c); printf("=== %d failure(s) ===\n", g_fail); return g_fail ? 1 : 0; }

        /* The HOME forge chunk (0,63,0) holds the pole axis (x=8,z=8): its centre
         * column must be solid crust (pole-flattened), or the forge carve breaks. */
        worldgen_fill_chunk(c, 0, 63, 0, 0);
        check("pole/forge chunk: centre column top is SOLID (flat crust under spawn)",
              vox_mat(c->voxels[vox_index(8, 15, 8)]) != MAT_AIR);

        /* A deep interior chunk is all STONE; a far exterior chunk is all AIR. */
        worldgen_fill_chunk(c, 0, 32, 0, 0);            /* world-Y ~512, near the core */
        check("deep core chunk is solid (no air)",
              vox_mat(c->voxels[vox_index(8, 8, 8)]) == MAT_STONE);
        check("worldgen_chunk_all_air agrees a far chunk is air",
              worldgen_chunk_all_air(40, 40, 40));
        check("worldgen_chunk_all_air: a core chunk is NOT all-air", !worldgen_chunk_all_air(0, 32, 0));
    }

    /* The relief never lets the surface exceed R+AMP (what all_air relies on). */
    {
        int over = 0;
        for (dx = -R; dx <= R; dx += 31)
            for (dz = -R; dz <= R; dz += 31) {
                int off = worldgen_radial_offset(dx, R / 2, dz);
                if (off > WG_RELIEF_AMP) over = 1;
            }
        check("surface displacement never exceeds +AMP (all-air predicate is safe)", !over);
    }

    printf("=== %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
