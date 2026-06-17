/* test_net.c - headless multiplayer protocol test (0.3).
 *
 * Runs a HOST and a CLIENT NetState in ONE process over loopback (127.0.0.1),
 * pumping both with net_poll(), and asserts the wire contract end-to-end:
 *   - the version-gated handshake completes and the client adopts the host seed,
 *   - poses round-trip into the avatar table BOTH directions,
 *   - edits round-trip (coords + voxel) BOTH directions,
 *   - a client built against a different game version is REFUSED.
 * No GL, no world - pure net.c + the Linux socket backend.
 */
#include <stdio.h>
#include <stdlib.h>   /* getenv */
#include "net.h"

static int g_fail = 0;
static void check(const char *name, int ok, const char *detail)
{
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s%s%s\n", name, detail ? " - " : "", detail ? detail : ""); g_fail++; }
}

/* net_drain_edits capture */
static int      g_dcount;
static int      g_dwx, g_dwy, g_dwz;
static uint32_t g_dvox;
static void capture_edit(int wx, int wy, int wz, uint32_t v, void *u)
{
    (void)u;
    g_dcount++; g_dwx = wx; g_dwy = wy; g_dwz = wz; g_dvox = v;
}

static void pump(NetState *a, NetState *b, int iters)
{
    int i;
    for (i = 0; i < iters; ++i) { net_poll(a); net_poll(b); net_sys_sleep_ms(1); }
}

/* Bind a host on the first free port in [base, base+30); returns the port or 0. */
static unsigned short host_on_free_port(NetState **out, uint64_t seed, uint32_t gv, uint32_t gen, int base)
{
    int p;
    for (p = base; p < base + 30; ++p) {
        NetState *h = net_host((unsigned short)p, seed, gv, gen);
        if (h) { *out = h; return (unsigned short)p; }
    }
    *out = NULL;
    return 0;
}

int main(void)
{
    const uint32_t GV = 0x000300u, GEN = 2u;
    const uint64_t SEED = 0xABCDEF1234567890ull;
    NetState *host = NULL, *client = NULL;
    unsigned short port;
    int i;

    printf("=== net (multiplayer protocol) tests ===\n");

    port = host_on_free_port(&host, SEED, GV, GEN, 28750);
    if (!host) { printf("SKIP: could not bind any test port (environment)\n"); return 0; }
    check("host created (id 0, ready, hosting)",
          net_mode(host) == NET_HOST && net_local_id(host) == 0 && net_ready(host), NULL);

    client = net_client("127.0.0.1", port, GV, GEN);
    check("client socket created", client != NULL, NULL);
    if (!client) { net_shutdown(host); printf("=== %d failure(s) ===\n", g_fail); return g_fail ? 1 : 0; }

    for (i = 0; i < 500 && !(net_ready(client) || net_failed(client)); ++i) {
        net_poll(host); net_poll(client); net_sys_sleep_ms(1);
    }
    check("client handshake completed", net_ready(client) && !net_failed(client), NULL);
    check("client adopted the host seed", net_seed(client) == SEED, NULL);
    check("client assigned id 1",         net_local_id(client) == 1, NULL);

    /* host pose -> the client should see the host (id 0) as an avatar */
    {
        float hp[3] = { 10.5f, 64.0f, 8.5f }, hh[3] = { 0.0f, 0.0f, -1.0f };
        float ap[3], ah[3], ac[3]; int aid;
        net_send_pose(host, hp, hh);
        pump(host, client, 5);
        check("client sees exactly 1 avatar (the host)", net_avatar_count(client) == 1, NULL);
        if (net_get_avatar(client, 0, ap, ah, &aid, ac)) {
            check("client avatar is host id 0", aid == 0, NULL);
            check("client avatar pos round-tripped",
                  ap[0] == hp[0] && ap[1] == hp[1] && ap[2] == hp[2], NULL);
            check("client avatar heading round-tripped",
                  ah[0] == hh[0] && ah[1] == hh[1] && ah[2] == hh[2], NULL);
        } else check("client net_get_avatar(0)", 0, "returned 0");
    }

    /* client pose -> the host should see the client (id 1) as an avatar */
    {
        float cp[3] = { -3.0f, 64.0f, 8.5f }, ch[3] = { 1.0f, 0.0f, 0.0f };
        float ap[3], ah[3], ac[3]; int aid;
        net_send_pose(client, cp, ch);
        pump(host, client, 5);
        check("host sees exactly 1 avatar (the client)", net_avatar_count(host) == 1, NULL);
        if (net_get_avatar(host, 0, ap, ah, &aid, ac))
            check("host avatar is client id 1 at the right pos",
                  aid == 1 && ap[0] == cp[0] && ap[1] == cp[1] && ap[2] == cp[2], NULL);
        else check("host net_get_avatar(0)", 0, "returned 0");
    }

    /* client edit -> the host applies it via drain */
    {
        g_dcount = 0;
        net_send_edit(client, 5, 64, -7, 0xDEADBEEFu);
        pump(host, client, 5);
        net_drain_edits(host, capture_edit, NULL);
        check("host drained exactly the client's 1 edit", g_dcount == 1, NULL);
        check("client->host edit round-tripped (coords + voxel)",
              g_dwx == 5 && g_dwy == 64 && g_dwz == -7 && g_dvox == 0xDEADBEEFu, NULL);
    }

    /* host edit -> the client applies it via drain (negative coords too) */
    {
        g_dcount = 0;
        net_send_edit(host, -2, 3, 4, 0x01020304u);
        pump(host, client, 5);
        net_drain_edits(client, capture_edit, NULL);
        check("client drained exactly the host's 1 edit", g_dcount == 1, NULL);
        check("host->client edit round-tripped (coords + voxel)",
              g_dwx == -2 && g_dwy == 3 && g_dwz == 4 && g_dvox == 0x01020304u, NULL);
    }

    /* drain with nothing pending must not invoke the callback */
    {
        g_dcount = 0;
        net_drain_edits(client, capture_edit, NULL);
        check("empty drain is a no-op", g_dcount == 0, NULL);
    }

    /* BACKPRESSURE regression: a burst of many valid poses arriving in one read
     * must NOT drop the link. (conn_recv used to kill a peer the instant rbuf
     * filled to NET_RECV_CAP, before parse_messages ran - so ~8 KB of perfectly
     * good frames disconnected an honest peer.) 500 poses = 14000 B > the 8192 B
     * recv buffer, so this fills + overruns it in one flush. */
    {
        float bp[3], bh[3] = { 0.0f, 1.0f, 0.0f };
        float ap[3], ah[3], ac[3]; int aid, k;
        for (k = 0; k < 500; ++k) { bp[0] = (float)k; bp[1] = 64.0f; bp[2] = 8.5f; net_send_pose(client, bp, bh); }
        pump(host, client, 12);
        check("host survived a 14 KB valid-pose burst (link alive)", net_avatar_count(host) == 1, NULL);
        if (net_get_avatar(host, 0, ap, ah, &aid, ac))
            check("host parsed the whole burst (last pose applied)", ap[0] == 499.0f, NULL);
        else check("host avatar after burst", 0, "client was dropped (backpressure kill)");
    }

    net_shutdown(client);
    net_shutdown(host);

    /* version-mismatch: a client built against a different game version is refused */
    {
        NetState *h2 = NULL, *c2 = NULL;
        unsigned short p2 = host_on_free_port(&h2, SEED, GV, GEN, 28790);
        if (h2) {
            c2 = net_client("127.0.0.1", p2, GV + 1u, GEN);   /* wrong game version */
            if (c2) {
                for (i = 0; i < 500 && !(net_ready(c2) || net_failed(c2)); ++i) {
                    net_poll(h2); net_poll(c2); net_sys_sleep_ms(1);
                }
                check("client with a mismatched build is REFUSED",
                      net_failed(c2) && !net_ready(c2), NULL);
                net_shutdown(c2);
            } else check("mismatch-test client created", 0, "net_client returned NULL");
            net_shutdown(h2);
        } else {
            printf("SKIP: version-mismatch case (no free port)\n");
        }
    }

    /* HOST IDLE-TIMEOUT (slot-squat DoS) regression. Opt-in (waits ~7s for the
     * handshake window): run with NET_TEST_IDLE=1. Fills all 3 client slots with
     * SILENT raw sockets (TCP-connected, never handshake), confirms a real client
     * is then refused, waits past the window so the host reaps the squatters, then
     * confirms a real client can join. */
    if (getenv("NET_TEST_IDLE")) {
        NetState *h = NULL;
        unsigned short hp = host_on_free_port(&h, SEED, GV, GEN, 28820);
        if (h) {
            net_sock sq[3];
            int j, q;
            double t0;
            for (j = 0; j < 3; ++j) sq[j] = net_sys_connect("127.0.0.1", hp);
            for (j = 0; j < 50; ++j) { net_poll(h); net_sys_sleep_ms(2); }  /* host accepts all 3 */

            {   /* slots full -> a real client is refused */
                NetState *r = net_client("127.0.0.1", hp, GV, GEN);
                for (q = 0; q < 300 && !(net_ready(r) || net_failed(r)); ++q) { net_poll(h); net_poll(r); net_sys_sleep_ms(2); }
                check("join refused while 3 idle sockets squat the slots", !net_ready(r), NULL);
                net_shutdown(r);
            }

            t0 = net_sys_now_ms();                                          /* wait past the idle window */
            while (net_sys_now_ms() - t0 < 6500.0) { net_poll(h); net_sys_sleep_ms(10); }

            {   /* squatters reaped -> a real client joins */
                NetState *r = net_client("127.0.0.1", hp, GV, GEN);
                for (q = 0; q < 600 && !(net_ready(r) || net_failed(r)); ++q) { net_poll(h); net_poll(r); net_sys_sleep_ms(2); }
                check("join succeeds after idle squatters are reaped", net_ready(r) && !net_failed(r), NULL);
                net_shutdown(r);
            }

            for (j = 0; j < 3; ++j) net_sys_close(sq[j]);
            net_shutdown(h);
        } else printf("SKIP: idle-timeout test (no free port)\n");
    }

    printf("=== %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
