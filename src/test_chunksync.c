/* test_chunksync.c - end-to-end chunk-delta sync (0.3), headless.
 *
 * Builds two independent WorldStores on the same seed (a "host" + a "client"),
 * edits the host, runs chunksync_serve -> chunksync_apply, and asserts the client
 * chunk now matches the host's - exercising the real serialize/apply round-trip
 * (incl. the unmodified-empty case and the worst-case full-chunk size) with no GL
 * and no sockets.
 */
#include <stdio.h>
#include "world.h"
#include "worldgen.h"
#include "chunksync.h"
#include "net.h"        /* NET_CHUNK_MAX (the delta buffer cap) */
#include "voxel.h"
#include "persist.h"   /* persist_canon */
#include "material.h"  /* MAT_* */

static int g_fail = 0;
static void check(const char *n, int ok) { if (ok) printf("PASS: %s\n", n); else { printf("FAIL: %s\n", n); ++g_fail; } }

static void gen_cb(Chunk *c, int cx, int cy, int cz, uint64_t seed, void *u)
{ (void)u; worldgen_fill_chunk(c, cx, cy, cz, seed); }

static Voxel mk(uint8_t m, uint8_t f)
{ Voxel v = 0; vox_set_mat(&v, m); vox_set_fill(&v, f); vox_set_temp_code(&v, temp_encode_c(20.0)); return v; }

static WorldStore HW, CW;                 /* large -> static (BSS) */
static unsigned char buf[NET_CHUNK_MAX];

int main(void)
{
    uint64_t seed = 0xC0FFEEu;
    WorldCallbacks c;
    ChunkSyncCtx hctx, cctx;
    int len, count;
    const int CX = 0, CY = 8, CZ = 0;     /* a chunk resident after world_prime(8,8) */

    printf("=== chunk-delta sync tests ===\n");
    c.gen = gen_cb; c.mesh_upload = NULL; c.slot_free = NULL; c.user = NULL;
    if (world_init(&HW, seed, &c) != 0 || world_init(&CW, seed, &c) != 0) {
        printf("FAIL: world_init\n"); return 2;
    }
    world_prime(&HW, 8.0f, 8.0f);
    world_prime(&CW, 8.0f, 8.0f);
    hctx.world = &HW; hctx.persist = NULL; hctx.seed = seed;
    cctx.world = &CW; cctx.persist = NULL; cctx.seed = seed;

    check("target chunk resident on host",   world_get(&HW, CX, CY, CZ) != NULL);
    check("target chunk resident on client", world_get(&CW, CX, CY, CZ) != NULL);

    /* (1) unmodified chunk -> empty delta (count 0, 14-byte header only) */
    len   = chunksync_serve(CX, CY, CZ, buf, (int)NET_CHUNK_MAX, &hctx);
    count = buf[12] | (buf[13] << 8);
    check("unmodified chunk serializes to count 0", len == 14 && count == 0);

    /* (2) three host edits round-trip; an unedited voxel is left alone */
    {
        int wx0 = CX*16+3,  wy0 = CY*16+2,  wz0 = CZ*16+5;    /* -> stone  */
        int wx1 = CX*16+8,  wy1 = CY*16+0,  wz1 = CZ*16+8;    /* -> air    */
        int wx2 = CX*16+10, wy2 = CY*16+4,  wz2 = CZ*16+2;    /* -> copper */
        int ux  = CX*16+1,  uy  = CY*16+12, uz  = CZ*16+1;    /* NOT edited */
        Voxel before_un;

        world_edit_voxel(&HW, wx0, wy0, wz0, mk(MAT_STONE, 15));
        world_edit_voxel(&HW, wx1, wy1, wz1, mk(MAT_AIR, 0));
        world_edit_voxel(&HW, wx2, wy2, wz2, mk(MAT_COPPER, 15));
        before_un = world_get_voxel(&CW, ux, uy, uz);

        len   = chunksync_serve(CX, CY, CZ, buf, (int)NET_CHUNK_MAX, &hctx);
        count = buf[12] | (buf[13] << 8);
        check("3 edits serialize to count 3", count == 3);
        check("delta length matches count",   len == 14 + 3 * 6);

        chunksync_apply(buf, len, &cctx);

        check("edit 0 (stone) propagated to client",
              persist_canon(world_get_voxel(&CW, wx0, wy0, wz0)) == persist_canon(world_get_voxel(&HW, wx0, wy0, wz0)));
        check("edit 1 (air) propagated to client",
              persist_canon(world_get_voxel(&CW, wx1, wy1, wz1)) == persist_canon(world_get_voxel(&HW, wx1, wy1, wz1)));
        check("edit 2 (copper) propagated to client",
              persist_canon(world_get_voxel(&CW, wx2, wy2, wz2)) == persist_canon(world_get_voxel(&HW, wx2, wy2, wz2)));
        check("unedited voxel untouched on client", world_get_voxel(&CW, ux, uy, uz) == before_un);
    }

    /* (3) worst case: rewrite an ENTIRE chunk -> 4096-voxel delta still fits + applies */
    {
        const int BX = 0, BY = 8, BZ = 1;
        int lx, ly, lz, mism = 0;
        if (world_get(&HW, BX, BY, BZ) != NULL && world_get(&CW, BX, BY, BZ) != NULL) {
            for (lz = 0; lz < 16; ++lz) for (ly = 0; ly < 16; ++ly) for (lx = 0; lx < 16; ++lx)
                world_edit_voxel(&HW, BX*16+lx, BY*16+ly, BZ*16+lz, mk(MAT_COPPER, 15));
            len   = chunksync_serve(BX, BY, BZ, buf, (int)NET_CHUNK_MAX, &hctx);
            count = buf[12] | (buf[13] << 8);
            check("full-chunk delta count == 4096", count == 4096);
            check("full-chunk delta fits NET_CHUNK_MAX", len == 14 + 4096 * 6 && len <= (int)NET_CHUNK_MAX);
            chunksync_apply(buf, len, &cctx);
            for (lz = 0; lz < 16 && !mism; ++lz) for (ly = 0; ly < 16 && !mism; ++ly) for (lx = 0; lx < 16 && !mism; ++lx)
                if (persist_canon(world_get_voxel(&CW, BX*16+lx, BY*16+ly, BZ*16+lz)) !=
                    persist_canon(world_get_voxel(&HW, BX*16+lx, BY*16+ly, BZ*16+lz))) mism = 1;
            check("full chunk applied voxel-for-voxel", !mism);
        } else {
            check("worst-case chunk resident", 0);
        }
    }

    world_shutdown(&HW);
    world_shutdown(&CW);
    printf("=== %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
