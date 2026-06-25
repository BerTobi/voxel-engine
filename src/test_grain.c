/* test_grain.c - 0.5 M0: assert the metres<->voxels macro algebra in units.h.
 *
 * No engine deps - just units.h. Forces units.h to COMPILE (firing its
 * _Static_assert) and checks each conversion folds to the expected value at the
 * 0.5 ship grain (k=2, 0.5 m/voxel). M2 expands this into the grep-gate audit
 * that EVERY physical constant in the engine routes through M2V/V2M. */
#include <stdio.h>
#include <math.h>
#include "units.h"
#include "worldgen.h"   /* 0.5 M2: WG_PLANET_* for the radial-gravity precision check */

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

    /* 0.5 M2 float-precision regression: radial gravity computes up =
     * normalize(pos - center) in FLOAT32 (player.c). At the new R=512 planet the
     * surface coords reach ~1024 vox; assert the worst-case float32 up-vector
     * error, expressed as a LATERAL displacement at the surface, stays well under
     * the collision tolerance - a guard so a future R bump that needs doubles
     * fails HERE, loudly, not as antipode jitter. Sweep many surface directions. */
    {
        const double cx = WG_PLANET_CX, cy = WG_PLANET_CY, cz = WG_PLANET_CZ;
        const double R  = WG_PLANET_R;
        double worst = 0.0;
        int i, j;
        for (i = 0; i <= 32; ++i) {
            double theta = 3.14159265358979 * (double)i / 32.0;      /* 0..pi   */
            for (j = 0; j < 64; ++j) {
                double phi = 6.28318530717959 * (double)j / 64.0;    /* 0..2pi  */
                double ux = sin(theta) * cos(phi), uy = cos(theta), uz = sin(theta) * sin(phi);
                double px = cx + R * ux, py = cy + R * uy, pz = cz + R * uz;
                /* FLOAT32 path, exactly as player.c stores pos + builds up */
                float  fx = (float)px - (float)cx, fy = (float)py - (float)cy, fz = (float)pz - (float)cz;
                float  fl = sqrtf(fx * fx + fy * fy + fz * fz);
                double aerr, lat;
                double ex, ey, ez;
                if (fl <= 0.0f) continue;
                ex = (double)(fx / fl) - ux;
                ey = (double)(fy / fl) - uy;
                ez = (double)(fz / fl) - uz;
                aerr = sqrt(ex * ex + ey * ey + ez * ez);   /* chord of the up-angle error */
                lat  = aerr * R;                            /* lateral voxels at the surface */
                if (lat > worst) worst = lat;
            }
        }
        printf("%s: worst radial-up float32 error = %.5f vox at R=%d (bound 0.25)\n",
               worst < 0.25 ? "PASS" : "FAIL", worst, (int)WG_PLANET_R);
        if (!(worst < 0.25)) fails++;
    }

    printf("=== %d failure(s) ===\n", fails);
    return fails;
}
