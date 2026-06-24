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

static int g_burst_n;
static void count_edit(int wx, int wy, int wz, uint32_t v, void *u)
{
    (void)wx; (void)wy; (void)wz; (void)v; (void)u;
    g_burst_n++;
}

/* chunk-sync plumbing: synthetic serve (a large, verifiable payload) + apply. */
#define CHUNK_TEST_LEN 20000
static int g_serve_calls, g_serve_cx;
static int test_serve(int cx, int cy, int cz, unsigned char *out, int cap, void *user)
{
    int i, n = CHUNK_TEST_LEN;
    (void)cy; (void)cz; (void)user;
    g_serve_calls++; g_serve_cx = cx;
    if (n > cap) n = cap;
    out[0]=(unsigned char)cx; out[1]=(unsigned char)(cx>>8); out[2]=(unsigned char)(cx>>16); out[3]=(unsigned char)(cx>>24);
    for (i = 4; i < n; ++i) out[i] = (unsigned char)(i * 7 + cx);
    return n;
}
static int g_apply_len, g_apply_ok, g_apply_cx, g_apply_calls;
static void test_apply(const unsigned char *data, int len, void *user)
{
    int i, ok = 1;
    (void)user;
    g_apply_calls++;
    g_apply_len = len;
    g_apply_cx = (int)((uint32_t)data[0] | ((uint32_t)data[1]<<8) | ((uint32_t)data[2]<<16) | ((uint32_t)data[3]<<24));
    for (i = 4; i < len; ++i) if (data[i] != (unsigned char)(i * 7 + g_apply_cx)) { ok = 0; break; }
    g_apply_ok = ok;
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

    /* BACKPRESSURE regression: a burst of many valid frames arriving in one read
     * must NOT drop the link. (conn_recv used to kill a peer the instant rbuf
     * filled to NET_RECV_CAP, before parse_messages ran - so ~8 KB of perfectly
     * good frames disconnected an honest peer.) EDITs are unthrottled (poses are
     * now ~33 Hz-capped), so 600 edits = 600*19 = 11400 B > the 8192 B recv
     * buffer, overrunning it in one flush. */
    {
        int k;
        { float bp[3] = { 1.0f, 64.0f, 8.5f }, bh[3] = { 0.0f, 1.0f, 0.0f };  /* one pose -> host has our avatar */
          net_send_pose(client, bp, bh); pump(host, client, 4); }
        for (k = 0; k < 600; ++k) net_send_edit(client, k, 64, 8, 0xAB000000u | (uint32_t)k);
        pump(host, client, 20);
        check("host survived an 11 KB valid-edit burst (link alive)", net_avatar_count(host) == 1, NULL);
        g_burst_n = 0;
        net_drain_edits(host, count_edit, NULL);
        check("host drained the entire burst (no frames lost)", g_burst_n == 600, NULL);
    }

    /* CHUNK-SYNC plumbing: a request routes to the host's serve callback, and its
     * large (~20 KB) reply reaches the client's apply callback byte-identical -
     * exercising MSG_CREQ/CDATA routing + variable-length framing of a frame far
     * bigger than one recv. */
    {
        net_set_chunk_server(host, test_serve, NULL);
        net_set_chunk_apply(client, test_apply, NULL);
        g_serve_calls = 0; g_apply_len = 0; g_apply_ok = 0;
        net_request_chunk(client, 4242, 7, 9);
        pump(host, client, 16);
        check("chunk request reached the host serve cb (right cx)", g_serve_calls == 1 && g_serve_cx == 4242, NULL);
        check("client got the 20 KB chunk frame byte-intact",
              g_apply_len == CHUNK_TEST_LEN && g_apply_cx == 4242 && g_apply_ok, NULL);
    }

    /* CHUNK-SYNC backpressure regression: a request batch whose replies (10 x
     * ~20 KB = 200 KB) far exceed the 64 KB send buffer must THROTTLE (serve over
     * several polls), never overflow + drop the client. (Pre-fix the host queued
     * them all in one poll and conn_queue killed the link.) */
    {
        int k;
        g_apply_calls = 0;
        for (k = 0; k < 10; ++k) net_request_chunk(client, 5000 + k, 0, 0);
        pump(host, client, 80);
        check("host did NOT drop the client under a 200 KB reply batch",
              net_ready(client) && !net_failed(client), NULL);
        check("all 10 oversized chunk replies were delivered (throttled, not dropped)",
              g_apply_calls == 10, NULL);
    }

    /* 0.4 M5: HOST-INITIATED chunk PUSH (the CA streaming channel). The host pushes
     * a delta UNSOLICITED (no client request) via net_host_push_chunk; it must reach
     * the client's apply callback byte-intact - the host-authoritative streaming
     * path clients render the living world through. */
    {
        unsigned char buf[CHUNK_TEST_LEN];
        int i;
        buf[0] = (unsigned char)1337; buf[1] = (unsigned char)(1337 >> 8);
        buf[2] = 0; buf[3] = 0;                       /* cx marker = 1337 */
        for (i = 4; i < CHUNK_TEST_LEN; ++i) buf[i] = (unsigned char)(i * 7 + 1337);
        g_apply_calls = 0; g_apply_len = 0; g_apply_ok = 0; g_apply_cx = 0;
        net_host_push_chunk(host, buf, CHUNK_TEST_LEN);
        pump(host, client, 16);
        check("host PUSH (unsolicited) reached the client apply cb byte-intact",
              g_apply_calls == 1 && g_apply_cx == 1337 &&
              g_apply_len == CHUNK_TEST_LEN && g_apply_ok, NULL);
    }

    net_shutdown(client);
    net_shutdown(host);

    /* MULTI-CLIENT (4-player): 1 host + 3 clients - distinct ids, everyone sees
     * everyone (relay), an edit reaches all peers EXCEPT its sender, 4th refused. */
    {
        NetState *mh = NULL, *mc[3];
        unsigned short mp;
        int j, q;
        mc[0] = mc[1] = mc[2] = NULL;
        mp = host_on_free_port(&mh, SEED, GV, GEN, 28810);
        if (mh) {
            int ready, ids_ok, seen[NET_MAX_PLAYERS];
            for (j = 0; j < 3; ++j) mc[j] = net_client("127.0.0.1", mp, GV, GEN);
            for (q = 0; q < 900; ++q) {
                ready = 0; net_poll(mh);
                for (j = 0; j < 3; ++j) { net_poll(mc[j]); if (net_ready(mc[j])) ready++; }
                if (ready == 3) break;
                net_sys_sleep_ms(1);
            }
            ready = 0; for (j = 0; j < 3; ++j) ready += net_ready(mc[j]) ? 1 : 0;
            check("all 3 clients handshaked", ready == 3, NULL);

            for (j = 0; j < (int)NET_MAX_PLAYERS; ++j) seen[j] = 0;
            ids_ok = 1;
            for (j = 0; j < 3; ++j) {
                int id = net_local_id(mc[j]);
                if (id >= 1 && id < (int)NET_MAX_PLAYERS && !seen[id]) seen[id] = 1;
                else ids_ok = 0;
            }
            check("clients got distinct ids in 1..3", ids_ok, NULL);

            /* one pose each (throttle lets the first through), let it propagate */
            { float p[3] = { 0.0f, 64.0f, 8.0f }, h[3] = { 0.0f, 1.0f, 0.0f }; net_send_pose(mh, p, h); }
            for (j = 0; j < 3; ++j) { float p[3] = { (float)(j + 1), 64.0f, 8.0f }, h[3] = { 0.0f, 1.0f, 0.0f }; net_send_pose(mc[j], p, h); }
            for (q = 0; q < 40; ++q) { net_poll(mh); for (j = 0; j < 3; ++j) net_poll(mc[j]); net_sys_sleep_ms(1); }
            check("host sees 3 client avatars", net_avatar_count(mh) == 3, NULL);
            { int allsee = 1; for (j = 0; j < 3; ++j) if (net_avatar_count(mc[j]) != 3) allsee = 0;
              check("each client sees 3 avatars (host + 2 peers)", allsee, NULL); }

            /* edit from client 0 -> host + clients 1,2 receive it; client 0 does NOT */
            net_send_edit(mc[0], 7, 7, 7, 0x77u);
            for (q = 0; q < 25; ++q) { net_poll(mh); for (j = 0; j < 3; ++j) net_poll(mc[j]); net_sys_sleep_ms(1); }
            { int a, b, c;
              g_burst_n = 0; net_drain_edits(mh,    count_edit, NULL); a = g_burst_n;
              g_burst_n = 0; net_drain_edits(mc[1], count_edit, NULL); b = g_burst_n;
              g_burst_n = 0; net_drain_edits(mc[2], count_edit, NULL); c = g_burst_n;
              check("host + the 2 non-sender clients each got the edit", a == 1 && b == 1 && c == 1, NULL);
              g_burst_n = 0; net_drain_edits(mc[0], count_edit, NULL);
              check("the sender did NOT get its own edit echoed back", g_burst_n == 0, NULL); }

            /* a 4th client is refused: only 3 client slots exist */
            { NetState *c4 = net_client("127.0.0.1", mp, GV, GEN);
              for (q = 0; q < 400 && !(net_ready(c4) || net_failed(c4)); ++q) { net_poll(mh); net_poll(c4); net_sys_sleep_ms(1); }
              check("a 4th client is refused (slots full)", !net_ready(c4), NULL);
              net_shutdown(c4); }

            for (j = 0; j < 3; ++j) net_shutdown(mc[j]);
            net_shutdown(mh);
        } else printf("SKIP: 4-player test (no free port)\n");
    }

    /* RING-RESET regression: when a player leaves and its id is reused, the new
     * occupant must NOT interpolate in from the previous occupant's stale samples.
     * A joins far away + fills its ring, leaves; B reuses the id and sends one
     * pose nearby - B's avatar must read exactly B's position, not a blend. */
    {
        NetState *rh = NULL;
        unsigned short rp = host_on_free_port(&rh, SEED, GV, GEN, 28840);
        if (rh) {
            NetState *a = net_client("127.0.0.1", rp, GV, GEN);
            int q, z, idA;
            for (q = 0; q < 600 && !(net_ready(a) || net_failed(a)); ++q) { net_poll(rh); net_poll(a); net_sys_sleep_ms(1); }
            idA = net_local_id(a);
            check("ring-reset: client A joined", net_ready(a), NULL);
            for (z = 0; z < 4; ++z) {                       /* fill A's ring far away */
                float p[3] = { 1000.0f, 1000.0f, 1000.0f }, h[3] = { 0.0f, 1.0f, 0.0f };
                net_send_pose(a, p, h);
                for (q = 0; q < 3; ++q) { net_poll(rh); net_poll(a); }
                net_sys_sleep_ms(35);                       /* > throttle interval */
            }
            net_shutdown(a);                                /* A leaves -> host reaps idA */
            for (q = 0; q < 60; ++q) { net_poll(rh); net_sys_sleep_ms(2); }
            {
                NetState *b = net_client("127.0.0.1", rp, GV, GEN);
                float ap[3], ah[3], ac[3]; int aid;
                for (q = 0; q < 600 && !(net_ready(b) || net_failed(b)); ++q) { net_poll(rh); net_poll(b); net_sys_sleep_ms(1); }
                check("ring-reset: B reused A's freed id", net_local_id(b) == idA, NULL);
                { float p[3] = { 5.0f, 64.0f, 8.0f }, h[3] = { 0.0f, 1.0f, 0.0f }; net_send_pose(b, p, h); }
                for (q = 0; q < 8; ++q) { net_poll(rh); net_poll(b); net_sys_sleep_ms(1); }
                if (net_get_avatar(rh, 0, ap, ah, &aid, ac))
                    check("reused-id avatar reads B's pos, no ghost from A", ap[0] == 5.0f && ap[1] == 64.0f, NULL);
                else check("host has B's avatar after rejoin", 0, "no avatar present");
                net_shutdown(b);
            }
            net_shutdown(rh);
        } else printf("SKIP: ring-reset test (no free port)\n");
    }

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
