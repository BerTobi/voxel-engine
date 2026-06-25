/* test_grain.c - 0.5 M0: assert the metres<->voxels macro algebra in units.h.
 *
 * No engine deps - just units.h. Forces units.h to COMPILE (firing its
 * _Static_assert) and checks each conversion folds to the expected value at the
 * 0.5 ship grain (k=2, 0.5 m/voxel). M2 expands this into the grep-gate audit
 * that EVERY physical constant in the engine routes through M2V/V2M. */
#include <stdio.h>
#include "units.h"

static int fails = 0;
static void ck(const char *name, double got, double want)
{
    double d = got - want; if (d < 0) d = -d;
    if (d > 1e-6) { printf("FAIL: %s = %g, want %g\n", name, got, want); fails++; }
    else          printf("PASS: %s = %g\n", name, got);
}

int main(void)
{
    printf("== test_grain (0.5 M0: units.h metres<->voxels, grain=%d mm) ==\n", VOX_GRAIN_MM);
    ck("METRES_PER_VOXEL",         METRES_PER_VOXEL, 0.5);
    ck("VOXELS_PER_METRE",         VOXELS_PER_METRE, 2.0);
    ck("M2V(1 m) == 2 voxels",     M2V(1.0f),  2.0);
    ck("V2M(2 voxels) == 1 m",     V2M(2.0f),  1.0);
    ck("M2V(0.45 m player r)",     M2V(0.45f), 0.9);   /* collision radius stays 0.45 m */
    ck("round-trip V2M(M2V(3.7))", V2M(M2V(3.7f)), 3.7);
    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
