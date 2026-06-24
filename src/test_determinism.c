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

int main(void)
{
    printf("== test_determinism (0.4 M0 CA determinism harness) ==\n");

    test_reproducible_and_meaningful();
    test_tickcount_not_cadence();

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
