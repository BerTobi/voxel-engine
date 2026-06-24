/* test_determinism.c - The GL-free CA DETERMINISM harness (0.4 M0).
 *
 * The substrate the multiplayer-CA determinism work (M5 host-authoritative
 * streaming, and the 0.5 lockstep flip) is VERIFIED on. It drives sim_tick()
 * DIRECTLY - no GL, no platform, no window, no sockets - exactly as
 * test_progress.c / test_sim.c do, because VOXEL_HOST/VOXEL_CONNECT through
 * main.c cannot serve as a determinism harness (main.c always creates a window +
 * GL context). The single-machine CA is hashed with sim_state_hash() (compiled
 * in ONLY here via -DVOXEL_DETERMINISM_HARNESS; absent from release binaries).
 *
 * AT M0 the asserted property is the trivial-but-load-bearing single-machine one:
 *   (1) REPRODUCIBLE: two byte-identical worlds ticked the same N hash equal.
 *   (2) MEANINGFUL: the hash actually CHANGES as the world evolves (so (1) is
 *       not a vacuous constant-hash pass).
 *   (3) TICK-COUNT, NOT CADENCE: a world ticked N-in-one-batch hashes equal to
 *       one ticked in two batches of N/2 - committed state is a function of the
 *       number of sim_tick() calls, not how they were grouped. (This is the
 *       property M3a's WorldClock and M5's host stream lean on.)
 *   (4) GENUINE PHYSICS: the rig actually melts copper (not a no-op world).
 *
 * NOT YET ASSERTED (documented so the harness is honest): the TWO-PEER
 * RENDER-FIDELITY property - a host runs the CA, pushes 8-bit deltas, a passive
 * client applies them and renders the same 8-bit canon - arrives in M5, when the
 * host-stream push/apply path exists. This file is the scaffold it slots into.
 *
 * Build (see Makefile `testdeterminism`):
 *   gcc -std=c99 -Wall -Wextra -Isrc -DVOXEL_DETERMINISM_HARNESS -o build/det_test \
 *       src/material.c src/chunk.c src/sim.c src/progress.c src/test_determinism.c -lm
 *   build/det_test
 */
#include <stdio.h>
#include <string.h>

#include "voxel.h"
#include "material.h"
#include "chunk.h"
#include "sim.h"

/* ---- Tiny assertion plumbing (same shape as test_sim.c / test_progress.c) -- */
static int g_failures = 0;

static int report(const char *name, int ok)
{
    if (ok) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++g_failures;
    }
    return ok;
}

#define CODE_LAVA   212u   /* temp_encode_c(1150 C) - the held-source code      */
#define MELT_TICKS  1200   /* enough for the central copper voxel to melt       */

/* Stand up one copper-melt world into *c and *s: a chunk full of copper at
 * ambient, with the six face neighbours of the central voxel held at the lava
 * code so the centre crosses copper's 1085 C melt point. Identical rig to
 * test_progress.c's setup_copper_melt_world (a known-melting scenario). Returns
 * the central copper voxel's local index. */
static uint16_t setup_copper_melt_world(SimState *s, Chunk *c)
{
    int hx = 8, hy = 8, hz = 8, n, i;
    uint16_t cu_li = (uint16_t)vox_index(hx, hy, hz);
    Voxel base = 0;

    memset(c, 0, sizeof *c);
    vox_set_mat(&base, MAT_COPPER);
    vox_set_fill(&base, FLUID_FULL);
    vox_set_temp_code(&base, 60u);   /* ambient: temp_encode_c(20 C) == 60 */
    for (i = 0; i < CHUNK_VOXELS; ++i)
        c->voxels[i] = base;

    sim_build_conduct_lut();
    sim_init(s, c);

    for (n = 0; n < 6; ++n) {
        int nx = hx + ((n == 0) - (n == 1));
        int ny = hy + ((n == 2) - (n == 3));
        int nz = hz + ((n == 4) - (n == 5));
        sim_set_source(s, (uint16_t)vox_index(nx, ny, nz), (uint8_t)CODE_LAVA);
    }
    return cu_li;
}

/* (1) + (2) + (4): two identical worlds ticked the same N hash equal; the hash
 * evolves from its initial value; the rig genuinely melts copper. */
static void test_reproducible_and_meaningful(void)
{
    SimState sa, sb;
    Chunk ca, cb;
    uint16_t cu_a, cu_b;
    uint64_t h0, ha, hb;
    int t;

    cu_a = setup_copper_melt_world(&sa, &ca);
    cu_b = setup_copper_melt_world(&sb, &cb);

    h0 = sim_state_hash(&sa);   /* state before any tick */

    for (t = 0; t < MELT_TICKS; ++t) { sim_tick(&sa); sim_tick(&sb); }

    ha = sim_state_hash(&sa);
    hb = sim_state_hash(&sb);

    report("reproducible: two identical worlds hash-equal after the same N ticks",
           ha == hb);
    report("meaningful: the state hash changed as the world evolved (not constant)",
           ha != h0);
    report("genuine physics: the central copper voxel actually melted",
           vox_mat(ca.voxels[cu_a]) == MAT_MOLTEN_COPPER &&
           vox_mat(cb.voxels[cu_b]) == MAT_MOLTEN_COPPER);

    sim_shutdown(&sa);
    sim_shutdown(&sb);
}

/* (3) TICK-COUNT, NOT CADENCE: committed state is a function of the number of
 * sim_tick() calls, not how they are batched. World A ticks N in one loop;
 * world B ticks N/2 then N/2. They must hash equal. This is the determinism
 * property the WorldClock (M3a) and host stream (M5) rely on: how many ticks
 * run is wall-clock-derived, but WHAT a given tick computes is not. */
static void test_tickcount_not_cadence(void)
{
    SimState sa, sb;
    Chunk ca, cb;
    int t;

    (void)setup_copper_melt_world(&sa, &ca);
    (void)setup_copper_melt_world(&sb, &cb);

    for (t = 0; t < MELT_TICKS; ++t) sim_tick(&sa);              /* one batch    */
    for (t = 0; t < MELT_TICKS / 2; ++t) sim_tick(&sb);         /* two batches  */
    for (t = 0; t < MELT_TICKS - MELT_TICKS / 2; ++t) sim_tick(&sb);

    report("tick-count not cadence: N-in-one-batch == (N/2 + N/2) batched",
           sim_state_hash(&sa) == sim_state_hash(&sb));

    sim_shutdown(&sa);
    sim_shutdown(&sb);
}

/* M3a: feeding the sim's tick_index from an EXTERNAL clock (the WorldClock
 * pattern main.c uses - set tick_index = world_tick before each tick, read it
 * back after) produces byte-identical state to letting sim_tick self-count.
 * This validates the exact M3a mechanism: the shared logical clock is equivalent
 * to the self-counter for one chunk, so M3a is byte-identical to 0.3. */
static void test_external_clock(void)
{
    SimState sa, sb;
    Chunk ca, cb;
    uint64_t wt = 0;
    int t;
    (void)setup_copper_melt_world(&sa, &ca);   /* A: sim_tick self-counts          */
    (void)setup_copper_melt_world(&sb, &cb);   /* B: externally clocked (WorldClock) */
    for (t = 0; t < MELT_TICKS; ++t) {
        sim_tick(&sa);
        sb.tick_index = wt;                    /* feed the shared clock            */
        sim_tick(&sb);
        wt = sb.tick_index;                    /* read the advanced clock back     */
    }
    report("M3a: external WorldClock feed == self-counted tick_index",
           sim_state_hash(&sa) == sim_state_hash(&sb));
    sim_shutdown(&sa);
    sim_shutdown(&sb);
}

/* ===== 0.4 M4: cross-chunk seam tests (GL-free) =====================------ *
 * Verify the read/commit split + cross-chunk boundary read: heat crosses a chunk
 * seam, the seam diffuses EXACTLY like an interior face (no seam artifact), and a
 * world tick is order-independent (the determinism the host stream relies on). */
typedef struct { SimState *sims[4]; int n; } SeamCtx;

/* SimNeighFn for the tests: read the adjacent chunk's start-of-tick heat[] + the
 * neighbour voxel's material (or decode an inactive neighbour's voxel temp). */
static int seam_nfn(void *user, const SimState *s, int face,
                    int nlx, int nly, int nlz, int32_t *out_heat, uint8_t *out_mat)
{
    SeamCtx *cx = (SeamCtx *)user;
    Chunk   *nc = s->chunk->neigh[face];
    int      i, nli;
    if (nc == NULL) return 0;                       /* closed wall */
    nli = vox_index(nlx, nly, nlz);
    for (i = 0; i < cx->n; ++i)
        if (cx->sims[i]->chunk == nc) {             /* active neighbour: live heat[] */
            *out_heat = cx->sims[i]->heat[nli];
            *out_mat  = vox_mat(nc->voxels[nli]);
            return 1;
        }
    *out_heat = temp_to_heat(vox_temp_code(nc->voxels[nli]));  /* inactive: voxel temp */
    *out_mat  = vox_mat(nc->voxels[nli]);
    return 1;
}

/* One world tick over a set of sims: feed the shared clock, READ all (cross-chunk
 * via seam_nfn), then COMMIT all - so every chunk reads start-of-tick state. */
static void seam_tick(SeamCtx *cx, uint64_t wt)
{
    int i;
    for (i = 0; i < cx->n; ++i) cx->sims[i]->tick_index = wt;
    for (i = 0; i < cx->n; ++i)
        sim_tick_ex(cx->sims[i], SIM_PHASE_READ, seam_nfn, NULL, cx);
    for (i = 0; i < cx->n; ++i)
        sim_tick_ex(cx->sims[i], SIM_PHASE_COMMIT, NULL, NULL, NULL);
}

static void fill_copper(Chunk *c, int cx, int cy, int cz)
{
    Voxel base = 0;
    int i;
    memset(c, 0, sizeof *c);
    vox_set_mat(&base, MAT_COPPER);
    vox_set_fill(&base, FLUID_FULL);
    vox_set_temp_code(&base, 60u);                  /* 20 C ambient */
    for (i = 0; i < CHUNK_VOXELS; ++i) c->voxels[i] = base;
    c->cx = cx; c->cy = cy; c->cz = cz;
}

static void test_seam_diffusion(void)
{
    SimState sInt, sA, sB;
    Chunk    cInt, cA, cB;
    SeamCtx  cInt1, cAB;
    int t;
    sim_build_conduct_lut();

    /* INTERIOR reference: one chunk, held source at (7,8,8). (8,8,8) is auto-woken
     * (adjacent to the source) and receives flux across the interior 7|8 face. */
    fill_copper(&cInt, 0, 0, 0);
    sim_init(&sInt, &cInt);
    sim_set_source(&sInt, (uint16_t)vox_index(7, 8, 8), 212u /*lava*/);
    sim_notify_edit(&sInt, vox_index(8, 8, 8));      /* wake the receiver identically to B */
    cInt1.sims[0] = &sInt; cInt1.n = 1;

    /* SEAM: chunk A (held source at its +X boundary 15,8,8) | chunk B = A's +X
     * neighbour. B's (0,8,8) then has the SAME neighbourhood as INT's (8,8,8): one
     * hot face (the source, across the seam) + 5 cold copper faces. We seed B(0,8,8)
     * active (the auto-wake across a seam is the wfn path, integration-tested). */
    fill_copper(&cA, 0, 0, 0);
    fill_copper(&cB, 1, 0, 0);
    cA.neigh[1] = &cB;   /* A +X (FACE_POS_X) = B */
    cB.neigh[0] = &cA;   /* B -X (FACE_NEG_X) = A */
    sim_init(&sA, &cA);
    sim_init(&sB, &cB);
    sim_set_source(&sA, (uint16_t)vox_index(15, 8, 8), 212u);
    sim_notify_edit(&sB, vox_index(0, 8, 8));        /* activate the seam voxel */
    cAB.sims[0] = &sA; cAB.sims[1] = &sB; cAB.n = 2;

    /* Compare EARLY (6 ticks): both receivers are 1 voxel from a held source with
     * identical 5-cold-face neighbourhoods, so they heat identically until a far
     * wall reflects - and the two chunks' forward chains are different lengths
     * (INT's +X wall is 7 voxels away, B's is 15), so we must read before that
     * reflection returns. 6 ticks: heat has reached ~6 voxels, no wall hit yet. */
    for (t = 0; t < 6; ++t) {
        seam_tick(&cInt1, (uint64_t)t);
        seam_tick(&cAB,   (uint64_t)t);
    }

    report("M4: heat crosses the chunk seam (B boundary warmed above ambient)",
           sB.heat[vox_index(0, 8, 8)] > temp_to_heat(61u));
    report("M4: seam diffuses like an interior face (B[0,8,8] == INT[8,8,8])",
           sB.heat[vox_index(0, 8, 8)] == sInt.heat[vox_index(8, 8, 8)]);
    report("M4: seam diffuses like an interior face (B[1,8,8] == INT[9,8,8])",
           sB.heat[vox_index(1, 8, 8)] == sInt.heat[vox_index(9, 8, 8)]);

    sim_shutdown(&sInt); sim_shutdown(&sA); sim_shutdown(&sB);
}

static void test_seam_order_independence(void)
{
    SimState a1, b1, a2, b2;
    Chunk    ca1, cb1, ca2, cb2;
    SeamCtx  fwd, rev;
    int t;
    sim_build_conduct_lut();

    fill_copper(&ca1, 0,0,0); fill_copper(&cb1, 1,0,0);
    ca1.neigh[1] = &cb1; cb1.neigh[0] = &ca1;
    sim_init(&a1, &ca1); sim_init(&b1, &cb1);
    sim_set_source(&a1, (uint16_t)vox_index(15,8,8), 212u);
    sim_notify_edit(&b1, vox_index(0,8,8));

    fill_copper(&ca2, 0,0,0); fill_copper(&cb2, 1,0,0);
    ca2.neigh[1] = &cb2; cb2.neigh[0] = &ca2;
    sim_init(&a2, &ca2); sim_init(&b2, &cb2);
    sim_set_source(&a2, (uint16_t)vox_index(15,8,8), 212u);
    sim_notify_edit(&b2, vox_index(0,8,8));

    fwd.sims[0] = &a1; fwd.sims[1] = &b1; fwd.n = 2;   /* tick order [A,B] */
    rev.sims[0] = &b2; rev.sims[1] = &a2; rev.n = 2;   /* tick order [B,A] */
    for (t = 0; t < 300; ++t) { seam_tick(&fwd, (uint64_t)t); seam_tick(&rev, (uint64_t)t); }

    report("M4: cross-chunk world tick is order-independent (heat[] + voxels match)",
           memcmp(a1.heat, a2.heat, sizeof a1.heat) == 0 &&
           memcmp(b1.heat, b2.heat, sizeof b1.heat) == 0 &&
           memcmp(ca1.voxels, ca2.voxels, sizeof ca1.voxels) == 0 &&
           memcmp(cb1.voxels, cb2.voxels, sizeof cb1.voxels) == 0);

    sim_shutdown(&a1); sim_shutdown(&b1); sim_shutdown(&a2); sim_shutdown(&b2);
}

int main(void)
{
    printf("== test_determinism (0.4 M0/M3a/M4 CA determinism harness) ==\n");

    test_reproducible_and_meaningful();
    test_tickcount_not_cadence();
    test_external_clock();
    test_seam_diffusion();
    test_seam_order_independence();

    /* Documented, NOT asserted at M0: the two-peer host->client render-fidelity
     * property (host runs the CA + pushes 8-bit deltas; a passive client applies
     * them and matches the host's 8-bit canon) arrives in M5. This harness is the
     * GL-free scaffold it slots into. */
    printf("NOTE: two-peer render-fidelity property is M5 (not asserted yet)\n");

    if (g_failures == 0)
        printf("=== 0 failure(s) ===\n");
    else
        printf("FAILURES: %d\n", g_failures);

    return g_failures;
}
