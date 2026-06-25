/* test_water_net.c - 0.5 M5: two-peer WATER render-fidelity over the wire.
 *
 * The headline 0.5 multiplayer guarantee: a CLIENT sees the host's living water.
 * Clients run NO cellular automaton (main.c gates the WorldCA on host/SP only);
 * they render whatever the host streams. This test drives that exact path with
 * the REAL pieces composed end-to-end - no GL, no mocks:
 *
 *   host WorldStore --(flood with water)--> chunksync_serve (RLE delta)
 *        --> net_host_push_chunk --[loopback socket]--> client MSG_CDATA
 *        --> chunksync_apply --> client WorldStore
 *
 * then asserts the client's voxels match the host's (water reconstructed) and the
 * RLE delta crossed the wire compact (the dam-break/flood bandwidth property).
 * test_net.c proves the transport byte-intact; test_chunksync.c proves the codec
 * round-trips in-process; this proves the COMPOSITION (real codec + real socket).
 */
#include <stdio.h>
#include "net.h"
#include "world.h"
#include "worldgen.h"
#include "chunksync.h"
#include "voxel.h"
#include "persist.h"   /* persist_canon */
#include "material.h"  /* MAT_* */

static int g_fail = 0;
static void check(const char *n, int ok) { if (ok) printf("PASS: %s\n", n); else { printf("FAIL: %s\n", n); ++g_fail; } }

static void gen_cb(Chunk *c, int cx, int cy, int cz, uint64_t seed, void *u)
{ (void)u; worldgen_fill_chunk(c, cx, cy, cz, seed); }
static int is_air_cb(int cx, int cy, int cz, uint64_t seed, void *u)
{ (void)seed; (void)u; return worldgen_chunk_all_air(cx, cy, cz); }

static Voxel water(void)
{ Voxel v = 0; vox_set_mat(&v, MAT_WATER); vox_set_fill(&v, 15); vox_set_temp_code(&v, temp_encode_c(20.0)); return v; }

static WorldStore HW, CW;                       /* large -> static (BSS) */
static unsigned char buf[NET_CHUNK_MAX];

/* Host serves through the real chunksync codec; client applies through it. */
static ChunkSyncCtx hctx, cctx;

static unsigned short host_on_free_port(NetState **out, uint64_t seed, uint32_t gv, uint32_t gen, int base)
{
    int p;
    for (p = base; p < base + 30; ++p) { NetState *h = net_host((unsigned short)p, seed, gv, gen); if (h) { *out = h; return (unsigned short)p; } }
    *out = NULL; return 0;
}

/* Serve chunk (cx,cy,cz) from the host world and push it to all clients. Returns
 * the delta byte length (the wire cost). */
static int host_stream_chunk(NetState *host, int cx, int cy, int cz)
{
    int plen = chunksync_serve(cx, cy, cz, buf, (int)NET_CHUNK_MAX, &hctx);
    if (plen > 0) net_host_push_chunk(host, buf, plen);
    return plen;
}

int main(void)
{
    const uint32_t GV = 0x000500u, GEN = 3u;     /* loopback: both sides must agree */
    const uint64_t SEED = 0x5EED1234ABCDull;
    WorldCallbacks c;
    NetState *host = NULL, *client = NULL;
    unsigned short port;
    int i, plen, lx, ly, lz;

    printf("=== M5 two-peer water render-fidelity tests ===\n");

    c.gen = gen_cb; c.mesh_upload = NULL; c.slot_free = NULL; c.user = NULL; c.is_air = is_air_cb;
    if (world_init(&HW, SEED, &c) != 0 || world_init(&CW, SEED, &c) != 0) { printf("FAIL: world_init\n"); return 2; }
    world_prime(&HW, 8.0f, 136.0f, 8.0f);
    world_prime(&CW, 8.0f, 136.0f, 8.0f);
    hctx.world = &HW; hctx.persist = NULL; hctx.seed = SEED;
    cctx.world = &CW; cctx.persist = NULL; cctx.seed = SEED;

    port = host_on_free_port(&host, SEED, GV, GEN, 28820);
    if (!host) { printf("SKIP: could not bind any test port (environment)\n"); world_shutdown(&HW); world_shutdown(&CW); return 0; }
    client = net_client("127.0.0.1", port, GV, GEN);
    if (!client) { printf("FAIL: net_client\n"); net_shutdown(host); world_shutdown(&HW); world_shutdown(&CW); return 2; }
    net_set_chunk_apply(client, chunksync_apply, &cctx);   /* client renders host's stream via real codec */

    for (i = 0; i < 600 && !(net_ready(client) || net_failed(client)); ++i) { net_poll(host); net_poll(client); net_sys_sleep_ms(1); }
    check("client handshake completed", net_ready(client) && !net_failed(client));
    check("client adopted host seed",   net_seed(client) == SEED);

    /* (1) FULL FLOOD: the host fills an entire chunk with water (the dam-break case)
     * and streams it. The client must reconstruct all-water, and the RLE delta must
     * cross the wire compact (one run, not 24 KB of per-voxel records). */
    {
        const int CX = 0, CY = 8, CZ = 1;
        int mism = 0;
        if (world_get(&HW, CX, CY, CZ) != NULL && world_get(&CW, CX, CY, CZ) != NULL) {
            for (lz = 0; lz < 16; ++lz) for (ly = 0; ly < 16; ++ly) for (lx = 0; lx < 16; ++lx)
                world_edit_voxel(&HW, CX*16+lx, CY*16+ly, CZ*16+lz, water());
            plen = host_stream_chunk(host, CX, CY, CZ);
            for (i = 0; i < 40; ++i) { net_poll(host); net_poll(client); net_sys_sleep_ms(1); }
            check("full-flood delta crossed the wire compact (<= 64 B)", plen > 0 && plen <= 64);
            for (lz = 0; lz < 16 && !mism; ++lz) for (ly = 0; ly < 16 && !mism; ++ly) for (lx = 0; lx < 16 && !mism; ++lx)
                if (persist_canon(world_get_voxel(&CW, CX*16+lx, CY*16+ly, CZ*16+lz)) !=
                    persist_canon(world_get_voxel(&HW, CX*16+lx, CY*16+ly, CZ*16+lz))) mism = 1;
            check("client reconstructed the flooded chunk voxel-for-voxel", !mism);
            check("a client voxel is now WATER",
                  vox_mat(world_get_voxel(&CW, CX*16+8, CY*16+8, CZ*16+8)) == MAT_WATER);
        } else { check("flood chunk resident on both peers", 0); }
    }

    /* (2) REALISTIC POOL: water fills only the bottom 4 layers of a chunk (a lake
     * floor); the rest stays solid. The client must show water exactly where the
     * host has it and the original block everywhere else. */
    {
        const int CX = 0, CY = 9, CZ = 0;
        Voxel host_top_before;
        int water_ok = 1, dry_ok = 1;
        if (world_get(&HW, CX, CY, CZ) != NULL && world_get(&CW, CX, CY, CZ) != NULL) {
            host_top_before = world_get_voxel(&HW, CX*16+8, CY*16+15, CZ*16+8);  /* untouched cell */
            for (lz = 0; lz < 16; ++lz) for (ly = 0; ly < 4; ++ly) for (lx = 0; lx < 16; ++lx)
                world_edit_voxel(&HW, CX*16+lx, CY*16+ly, CZ*16+lz, water());
            plen = host_stream_chunk(host, CX, CY, CZ);
            for (i = 0; i < 40; ++i) { net_poll(host); net_poll(client); net_sys_sleep_ms(1); }
            check("pool delta sent (some runs)", plen > 14);
            for (lz = 0; lz < 16; ++lz) for (ly = 0; ly < 4; ++ly) for (lx = 0; lx < 16; ++lx)
                if (vox_mat(world_get_voxel(&CW, CX*16+lx, CY*16+ly, CZ*16+lz)) != MAT_WATER) water_ok = 0;
            check("client shows water in all 4 flooded layers", water_ok);
            /* a cell the host never touched must equal the host's (unchanged) value */
            if (persist_canon(world_get_voxel(&CW, CX*16+8, CY*16+15, CZ*16+8)) != persist_canon(host_top_before)) dry_ok = 0;
            check("client left the un-flooded cells untouched", dry_ok);
        } else { check("pool chunk resident on both peers", 0); }
    }

    /* (3) DRAIN desync regression: a cell the host changes from non-seed BACK to its
     * seed value is OMITTED from the (delta-from-seed) push, so an additive client
     * apply would keep phantom water there forever. The client must reset-to-seed on
     * apply and clear it. (This is the bug the 0.5 bug-hunt found in chunksync_apply.) */
    {
        const int CX = 0, CY = 9, CZ = 1;
        if (world_get(&HW, CX, CY, CZ) != NULL && world_get(&CW, CX, CY, CZ) != NULL) {
            int wx = CX*16+5, wy = CY*16+5, wz = CZ*16+5;
            Voxel seedv = world_get_voxel(&HW, wx, wy, wz);   /* pristine seed voxel */
            world_edit_voxel(&HW, wx, wy, wz, water());       /* host floods one cell  */
            host_stream_chunk(host, CX, CY, CZ);
            for (i = 0; i < 40; ++i) { net_poll(host); net_poll(client); net_sys_sleep_ms(1); }
            check("client received the streamed water cell",
                  vox_mat(world_get_voxel(&CW, wx, wy, wz)) == MAT_WATER);
            world_edit_voxel(&HW, wx, wy, wz, seedv);         /* host drains it back to SEED */
            host_stream_chunk(host, CX, CY, CZ);              /* delta now OMITS this cell   */
            for (i = 0; i < 40; ++i) { net_poll(host); net_poll(client); net_sys_sleep_ms(1); }
            check("client CLEARED the drained cell (no phantom water; reset-to-seed)",
                  persist_canon(world_get_voxel(&CW, wx, wy, wz)) == persist_canon(seedv));
        } else { check("drain-regression chunk resident on both peers", 0); }
    }

    net_shutdown(client);
    net_shutdown(host);
    world_shutdown(&HW);
    world_shutdown(&CW);
    printf("=== %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
