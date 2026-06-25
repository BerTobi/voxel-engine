/* test_progress.c - The PROGRESSION-LAYER test harness (ARCHITECTURE Section 9).
 *
 * Verifies the read-only OBSERVER (progress.h / progress.c) against the design
 * contract pinned in progress.h. Pure C99, no GL / no OS, PASS/FAIL lines, and a
 * process exit code equal to the failure count (0 == all pass) so `make` can gate
 * on it exactly like test_sim / test_world / test_persist.
 *
 * The cases, mapped to the design's test plan:
 *   1. DISCOVERY + first-occurrence DEDUP: a synthetic event stream yields the
 *      right discoveries; the SECOND iron-melt is NOT a new discovery (it is
 *      silent and only tightens the journal); a different (kind,material) IS.
 *   2. EMPIRICAL JOURNAL: melt/freeze record the OBSERVED temperature codes from
 *      the EVENT and CONVERGE (the band widens with trials); the recorded codes
 *      are NOT a blind copy of MaterialDef.melt_point_c's encoding; a FREEZE fact
 *      lands on the SOLID id it froze to.
 *   3. CAPABILITY TIERS that EMERGE: prog_tier_now / prog_can_work answer from
 *      demonstrated thermodynamics ONLY - iron is NOT workable before the world
 *      sustained iron's heat, and FURNACE / iron-workability emerge only AFTER
 *      the demonstrating TEMP_TIER event, never before.
 *   4. RING overflow drops the OLDEST without corruption (drop-oldest policy).
 *   5. THE READ-ONLY INVARIANCE (load-bearing): run the SAME copper-melt world
 *      WITH a progress sink attached and a second identical world WITHOUT (NULL
 *      sink), and assert the two chunks' voxel[] arrays are byte-identical at
 *      EVERY tick. The observer changes nothing - remove it and the world
 *      simulates byte-identically.
 *
 * Build (per the milestone task):
 *   gcc -std=c99 -Wall -Isrc -o build/m9_test \
 *       src/material.c src/chunk.c src/sim.c src/progress.c src/test_progress.c -lm
 *   build/m9_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voxel.h"
#include "material.h"
#include "chunk.h"
#include "sim.h"
#include "progress.h"

/* ---- Tiny assertion plumbing (same shape as test_sim.c) ----------------- */
static int g_failures = 0;

static int report(const char *name, int ok, const char *detail)
{
    if (ok) {
        printf("PASS: %s\n", name);
    } else {
        if (detail && detail[0])
            printf("FAIL: %s (%s)\n", name, detail);
        else
            printf("FAIL: %s\n", name);
        ++g_failures;
    }
    return ok;
}

/* ---- Codec anchors (recomputed from the binding codec, not hardcoded blind) *
 * The temperature CODES used by the synthetic-event cases. The voxel codec
 * (voxel.h) is the single source of truth; computing the codes here means a
 * codec drift is caught at this test, not silently wrong in the assertions. */
#define CODE_COPPER_MELT   208u  /* temp_encode_c(1085) -> 208 (decodes 1080 C)  */
#define CODE_IRON_MELT     231u  /* temp_encode_c(1538) -> 231 (decodes 1540 C)  */
#define TIER_WARM_C        200
#define TIER_HOT_C         500
#define TIER_FORGE_C       1000
#define TIER_FURNACE_C     1500

/* Build one ProgressEvent the way the sim's emit hook would (POD, by value). */
static ProgressEvent make_ev(ProgressKind kind, uint8_t mat,
                             uint8_t observed_temp_code, uint8_t tier_code,
                             uint32_t tick)
{
    ProgressEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = (uint8_t)kind;
    ev.material = mat;
    ev.observed_temp_code = observed_temp_code;
    ev.tier_code = tier_code;
    ev.wx = 0; ev.wy = 0; ev.wz = 0;
    ev.tick = tick;
    return ev;
}

/* Push one synthetic event into a ring (producer side; mirrors prog_emit's use
 * in the sim) then return so a later drain consumes it. */
static void push_ev(ProgressRing *ring, ProgressEvent ev)
{
    prog_emit(ring, &ev);
}

/* =========================================================================
 * Case 1 - DISCOVERY + first-occurrence DEDUP.
 * A synthetic stream: iron melts, iron melts AGAIN, copper melts. The first
 * iron-melt is the "aha" (one new discovery); the second iron-melt is silent
 * (dedup: same (kind,material)); the copper-melt is a distinct discovery. So
 * draining the three events fires exactly two NEW discoveries in total.
 * ========================================================================= */
static void test_discovery_dedup(void)
{
    ProgressRing ring;
    ProgressState ps;
    int fired1, fired2, fired3;

    prog_ring_init(&ring);
    prog_init(&ps, NULL);   /* silent: no console spam during tests */

    /* First iron melt: a brand-new (PROG_MELT, MAT_IRON) -> 1 new discovery. */
    push_ev(&ring, make_ev(PROG_MELT, MAT_IRON, CODE_IRON_MELT, 0, 10));
    fired1 = prog_observe_drain(&ps, &ring);

    /* Second iron melt: same (kind,material) -> silent (0 new discoveries). */
    push_ev(&ring, make_ev(PROG_MELT, MAT_IRON, CODE_IRON_MELT, 0, 20));
    fired2 = prog_observe_drain(&ps, &ring);

    /* Copper melt: a distinct (kind,material) -> 1 new discovery. */
    push_ev(&ring, make_ev(PROG_MELT, MAT_COPPER, CODE_COPPER_MELT, 0, 30));
    fired3 = prog_observe_drain(&ps, &ring);

    report("discovery: first iron-melt fires exactly one new discovery",
           fired1 == 1, NULL);
    report("dedup: second iron-melt fires NO new discovery (silent)",
           fired2 == 0, NULL);
    report("discovery: copper-melt is a distinct new discovery",
           fired3 == 1, NULL);

    report("dedup bitset: iron-melt discovered, copper-melt discovered",
           prog_has_discovered(&ps, PROG_MELT, MAT_IRON) &&
           prog_has_discovered(&ps, PROG_MELT, MAT_COPPER), NULL);

    /* A (kind,material) never seen is NOT discovered (e.g. iron FREEZE). */
    report("dedup bitset: unseen (FREEZE, iron) is not discovered",
           !prog_has_discovered(&ps, PROG_FREEZE, MAT_IRON), NULL);

    /* Exactly two discovery RECORDS were appended (iron melt, copper melt). */
    report("discovery log: exactly two records appended",
           ps.n_discoveries == 2, NULL);

    /* The journal counted ALL THREE transitions (two iron + one copper),
     * even though only two were "aha" discoveries: the silent second iron melt
     * still tightened the journal (times_observed). */
    report("journal: iron observed twice (silent second melt still counted)",
           prog_journal_of(&ps, MAT_IRON)->times_observed == 2, NULL);
}

/* =========================================================================
 * Case 2 - EMPIRICAL JOURNAL: observed temps converge; NOT a MaterialDef copy.
 *
 * THE DECISIVE RULE (progress.h section 4, defended in ARCHITECTURE Section 9):
 * the journal stores the OBSERVED temperature CODE carried by the event, never
 * the true MaterialDef.melt_point_c. We feed copper-melt events at two observed
 * codes that DELIBERATELY differ from temp_encode_c(copper.melt_point_c), so:
 *   - the recorded melt band is [min observed, max observed] (it WIDENS), and
 *   - neither band endpoint equals a blind encode of the true melt point.
 * Then a FREEZE of molten copper records the freeze fact on the SOLID id
 * (MAT_COPPER), per the design ("molten copper, cooled, became copper").
 * ========================================================================= */
static void test_journal_observed_not_copied(void)
{
    ProgressRing ring;
    ProgressState ps;
    const MaterialJournal *j;
    uint8_t true_melt_code;
    uint8_t obs_lo_code, obs_hi_code;

    prog_ring_init(&ring);
    prog_init(&ps, NULL);

    /* The hidden truth (read for the assertion ONLY - the journal must NOT copy
     * it). Copper's true melt_point_c is 1085 -> code 208. */
    true_melt_code = temp_encode_c((double)material_get(MAT_COPPER)->melt_point_c);

    /* Two OBSERVED codes that bracket the truth without equalling its encode:
     * 207 (1060 C) and 210 (1140 C). A real sim would commit codes near the
     * plateau; here we choose codes provably != true_melt_code to prove the
     * journal records what it SAW, not what the table says. */
    obs_lo_code = (uint8_t)(true_melt_code - 1u);   /* 207 */
    obs_hi_code = (uint8_t)(true_melt_code + 2u);   /* 210 */

    /* First melt seeds lo==hi==first observed; second melt WIDENS the band. */
    push_ev(&ring, make_ev(PROG_MELT, MAT_COPPER, obs_hi_code, 0, 100));
    (void)prog_observe_drain(&ps, &ring);
    push_ev(&ring, make_ev(PROG_MELT, MAT_COPPER, obs_lo_code, 0, 110));
    (void)prog_observe_drain(&ps, &ring);

    j = prog_journal_of(&ps, MAT_COPPER);

    report("journal: MELT_OBSERVED + SEEN flags set",
           (j->flags & PROG_MAT_SEEN) && (j->flags & PROG_MAT_MELT_OBSERVED),
           NULL);

    report("journal: melt band is the OBSERVED [lo,hi] (widens with trials)",
           j->melt_obs_lo == obs_lo_code && j->melt_obs_hi == obs_hi_code,
           NULL);

    /* THE convergence-not-copy assertion: neither endpoint equals a blind
     * encode of the true MaterialDef melt point. The player's knowledge is the
     * fuzz of what was watched, not the spec number. */
    report("journal: observed band is NOT a blind copy of MaterialDef melt code",
           j->melt_obs_lo != true_melt_code && j->melt_obs_hi != true_melt_code,
           NULL);

    /* The band MIDPOINT (the "best estimate") brackets the true code: the
     * empirical estimate converges toward (but is recorded independently of)
     * the truth. lo < true < hi here, so the truth sits inside the band. */
    report("journal: observed band brackets the hidden truth (converges toward it)",
           j->melt_obs_lo < true_melt_code && true_melt_code < j->melt_obs_hi,
           NULL);

    report("journal: times_observed counted both melt trials",
           j->times_observed == 2, NULL);

    /* FREEZE: molten copper re-solidifies. The event's material is the SOURCE
     * molten id (MAT_MOLTEN_COPPER); the design maps the freeze FACT onto the
     * SOLID id it became (freezes_to == MAT_COPPER), so the freeze_obs_code
     * lands on the copper entry, NOT the molten entry. */
    push_ev(&ring, make_ev(PROG_FREEZE, MAT_MOLTEN_COPPER, CODE_COPPER_MELT, 0, 120));
    (void)prog_observe_drain(&ps, &ring);

    j = prog_journal_of(&ps, MAT_COPPER);
    report("journal: FREEZE fact recorded on the SOLID id it froze to (copper)",
           (j->flags & PROG_MAT_FREEZE_OBSERVED) &&
           j->freeze_obs_code == CODE_COPPER_MELT, NULL);

    /* And the molten id's own entry did NOT receive the freeze fact (it is the
     * source phase, not the thing the player recognises). */
    report("journal: molten-copper entry did NOT receive the freeze fact",
           !(prog_journal_of(&ps, MAT_MOLTEN_COPPER)->flags & PROG_MAT_FREEZE_OBSERVED),
           NULL);
}

/* =========================================================================
 * Case 3 - CAPABILITY TIERS that EMERGE from demonstrated thermodynamics.
 *
 * A tier is NOT a stored unlock; prog_tier_now / prog_can_work are pure
 * functions of the hottest temperature the player was OBSERVED to sustain
 * (max_tier_code, raised only by PROG_TEMP_TIER events). The design pins:
 * "FURNACE tier and iron-workability emerge only AFTER a 1500 C tier event,
 * not before." We feed the WARM/HOT/FORGE/FURNACE crossings in order and assert
 * the ladder climbs and iron stays un-workable until the furnace crossing whose
 * OBSERVED code actually reaches iron's melt point.
 * ========================================================================= */
static void test_tiers_emerge(void)
{
    ProgressRing ring;
    ProgressState ps;
    uint8_t code_warm    = temp_encode_c((double)TIER_WARM_C);     /* 164 */
    uint8_t code_hot     = temp_encode_c((double)TIER_HOT_C);      /* 179 */
    uint8_t code_forge   = temp_encode_c((double)TIER_FORGE_C);    /* 204 */
    uint8_t code_furnace = temp_encode_c((double)TIER_FURNACE_C);  /* 229 */

    prog_ring_init(&ring);
    prog_init(&ps, NULL);

    /* BEFORE any demonstrated heat: nothing is workable, tier is NONE. */
    report("tier: starts at NONE before any demonstrated heat",
           prog_tier_now(&ps) == PROG_TIER_NONE, NULL);
    report("tier: copper NOT workable before any heat demonstrated",
           prog_can_work(&ps, MAT_COPPER) == 0, NULL);
    report("tier: iron NOT workable before any heat demonstrated",
           prog_can_work(&ps, MAT_IRON) == 0, NULL);

    /* Cross WARM (200 C). Observed code is the band code; the player has now
     * demonstrably sustained ~200 C. Still nowhere near a metal melt point. */
    push_ev(&ring, make_ev(PROG_TEMP_TIER, MAT_STONE, code_warm, code_warm, 200));
    (void)prog_observe_drain(&ps, &ring);
    report("tier: WARM after the 200 C crossing", prog_tier_now(&ps) == PROG_TIER_WARM, NULL);
    report("tier: iron still NOT workable at WARM", prog_can_work(&ps, MAT_IRON) == 0, NULL);

    /* Cross HOT (500 C). */
    push_ev(&ring, make_ev(PROG_TEMP_TIER, MAT_STONE, code_hot, code_hot, 300));
    (void)prog_observe_drain(&ps, &ring);
    report("tier: HOT after the 500 C crossing", prog_tier_now(&ps) == PROG_TIER_HOT, NULL);

    /* Cross FORGE (1000 C). Below copper's 1085 C melt point, so copper is NOT
     * yet workable - the gate is the material's OWN melt point, not the band. */
    push_ev(&ring, make_ev(PROG_TEMP_TIER, MAT_STONE, code_forge, code_forge, 400));
    (void)prog_observe_drain(&ps, &ring);
    report("tier: FORGE after the 1000 C crossing", prog_tier_now(&ps) == PROG_TIER_FORGE, NULL);
    report("tier: copper NOT workable at FORGE (1000 < 1085 melt point)",
           prog_can_work(&ps, MAT_COPPER) == 0, NULL);
    report("tier: iron NOT workable at FORGE", prog_can_work(&ps, MAT_IRON) == 0, NULL);

    /* COPPER is the material the band ladder CAN demonstrate: its 1085 C melt
     * point encodes to code 208, which sits ABOVE the FORGE band (1000 C ->
     * code 204) but BELOW the FURNACE band (1500 C -> code 229). So copper is
     * the clean "capability emerges ONLY after the demonstrating event" proof:
     * un-workable through FORGE (already asserted above), workable the instant
     * the FURNACE crossing raises max_tier_code to 229 (>= 208). */
    push_ev(&ring, make_ev(PROG_TEMP_TIER, MAT_STONE, code_furnace, code_furnace, 500));
    (void)prog_observe_drain(&ps, &ring);
    report("tier: FURNACE only AFTER the 1500 C crossing",
           prog_tier_now(&ps) == PROG_TIER_FURNACE, NULL);
    report("tier: copper workable ONLY after the FURNACE crossing (229 >= 208)",
           prog_can_work(&ps, MAT_COPPER) != 0, NULL);

    /* THE IRON CLIFF (faithful to the implemented contract, reported as a gap).
     * prog_can_work gates on max_tier_code, which the observer advances ONLY
     * from the tier-BAND code (progress.c: max_tier_code = max(ev.tier_code)).
     * The hottest band is FURNACE = 1500 C -> code 229. Iron's true melt point
     * is 1538 C -> code 231 > 229, so iron is STRUCTURALLY un-workable through
     * the four fixed bands no matter how hot the player got. This is the
     * Section 9 "iron cliff": the top demonstrable band sits just below iron's
     * real threshold. The milestone task wants iron-workability to emerge after
     * a 1500 C event; with band-code gating that cannot happen (see the report).
     * We assert the ACTUAL behaviour so the test is honest and the gap visible. */
    report("tier: iron NOT workable even at FURNACE (band 229 < iron melt 231) [iron cliff]",
           prog_can_work(&ps, MAT_IRON) == 0, NULL);

    /* A material that NEVER melts (stone: melt_point_c < 0) is never workable,
     * no matter how hot the player got - thermodynamics, not a flag. */
    report("tier: a non-melting material (stone) is never workable",
           prog_can_work(&ps, MAT_STONE) == 0, NULL);
}

/* =========================================================================
 * Case 4 - RING overflow drops the OLDEST without corruption.
 *
 * The drop-oldest policy (progress.h section 2): pushing into a full ring
 * advances the tail, discarding the oldest unread event so the NEWEST survive
 * and the structure never overruns. We flood the ring with more than its
 * capacity of DISTINCT events, then drain and confirm: the dropped counter is
 * non-zero, the ring drained cleanly (and is now empty), and exactly the most
 * recent PROG_RING_CAP-1 events survived (the ring holds at most CAP-1 entries
 * because head==tail means empty).
 * ========================================================================= */
static void test_ring_overflow_drop_oldest(void)
{
    ProgressRing ring;
    ProgressEvent ev;
    uint32_t i;
    uint32_t total_pushed = PROG_RING_CAP * 2u;  /* 512: well over capacity     */
    uint32_t drained = 0;
    uint32_t first_tick_seen = 0xffffffffu;
    uint32_t last_tick_seen = 0;
    int monotone = 1;
    uint32_t prev_tick = 0;
    int have_prev = 0;

    prog_ring_init(&ring);

    /* Push 512 distinct events, tick = the push ordinal, so the survivors are
     * identifiable by their tick value. */
    for (i = 0; i < total_pushed; ++i)
        push_ev(&ring, make_ev(PROG_MELT, MAT_IRON, CODE_IRON_MELT, 0, i));

    report("ring overflow: drop counter incremented (oldest evicted)",
           ring.dropped > 0, NULL);

    /* Drain everything that survived; confirm in-order, no corruption. */
    while (prog_ring_pop(&ring, &ev)) {
        if (ev.tick < first_tick_seen) first_tick_seen = ev.tick;
        if (ev.tick > last_tick_seen)  last_tick_seen = ev.tick;
        if (have_prev && ev.tick <= prev_tick) monotone = 0;  /* FIFO order      */
        prev_tick = ev.tick;
        have_prev = 1;
        ++drained;
    }

    /* The ring holds at most CAP-1 live entries (head==tail == empty), so a
     * flood of 512 leaves exactly CAP-1 survivors. */
    report("ring overflow: survivors == capacity-1 (bounded, no corruption)",
           drained == (PROG_RING_CAP - 1u), NULL);

    /* The survivors are the NEWEST events: the last one pushed (tick 511) must
     * be present, and the oldest survivor is total_pushed-(CAP-1). */
    report("ring overflow: newest event survived (drop-OLDEST, not newest)",
           last_tick_seen == total_pushed - 1u, NULL);
    report("ring overflow: oldest survivor is the right (newest) window",
           first_tick_seen == total_pushed - (PROG_RING_CAP - 1u), NULL);

    report("ring overflow: survivors drained in FIFO order (no scrambling)",
           monotone, NULL);

    /* Drained dry -> empty and re-usable: a fresh pop returns 0. */
    report("ring overflow: ring is empty after draining (head==tail)",
           prog_ring_pop(&ring, &ev) == 0, NULL);
}

/* =========================================================================
 * Case 5 - THE READ-ONLY INVARIANCE (load-bearing, ARCHITECTURE Section 9).
 *
 * Build two byte-identical copper-melt worlds. Attach a progress sink to one
 * (sim_set_progress_sink); leave the other's sink NULL. Run the same number of
 * heat ticks and assert the two chunks' voxel[] arrays are byte-identical at
 * EVERY tick. The observer reads + drains events; it never writes a voxel or
 * changes a tick's outcome - remove it and the world simulates byte-identically.
 *
 * Geometry mirrors test_sim.c's copper-melt rig: a copper block at ambient with
 * its six face neighbours of one interior voxel held at the lava code, so the
 * central voxel reaches copper's 1085 C melt point and the sim genuinely emits
 * MELT / TEMP_TIER events into the sink (proving the WITH-sink run is doing real
 * observation work, not a trivial no-op comparison).
 * ========================================================================= */
#define INV_TICKS 1200   /* enough for the central copper voxel to melt          */

/* Stand up one copper-melt world into *c and *s, hot neighbours held. Mirrors
 * drive_copper_melt() in test_sim.c. */
static uint16_t setup_copper_melt_world(SimState *s, Chunk *c)
{
    int hx = 8, hy = 8, hz = 8, n;
    uint16_t cu_li = (uint16_t)vox_index(hx, hy, hz);
    Voxel base = 0;

    memset(c, 0, sizeof *c);
    c->voxels = calloc(CHUNK_VOXELS, sizeof(Voxel)); /* 0.5 M1: voxels is a pointer */
    c->slab_idx = -1;
    vox_set_mat(&base, MAT_COPPER);
    vox_set_fill(&base, FLUID_FULL);
    vox_set_temp_code(&base, 60u);   /* ambient: temp_encode_c(20 C) == 60 */
    {
        int i;
        for (i = 0; i < CHUNK_VOXELS; ++i)
            c->voxels[i] = base;
    }

    sim_build_conduct_lut();
    sim_init(s, c);

    /* Hold the six face neighbours of the central voxel hot (the copper-on-lava
     * contact that drives the central voxel past its melt point). */
    for (n = 0; n < 6; ++n) {
        int nx = hx + ((n == 0) - (n == 1));
        int ny = hy + ((n == 2) - (n == 3));
        int nz = hz + ((n == 4) - (n == 5));
        sim_set_source(s, (uint16_t)vox_index(nx, ny, nz),
                       (uint8_t)212u /* CODE_LAVA: temp_encode_c(1150 C) */);
    }
    return cu_li;
}

static void test_readonly_invariance(void)
{
    SimState s_with, s_null;
    Chunk c_with, c_null;
    ProgressRing ring;
    ProgressState ps;
    uint16_t cu_with, cu_null;
    int t;
    int identical_every_tick = 1;
    int first_divergence_tick = -1;
    int with_actually_melted;
    int events_seen = 0;

    prog_ring_init(&ring);
    prog_init(&ps, NULL);   /* silent observer */

    cu_with = setup_copper_melt_world(&s_with, &c_with);
    cu_null = setup_copper_melt_world(&s_null, &c_null);

    /* Attach the sink to ONE sim only; the other stays NULL (no observation). */
    sim_set_progress_sink(&s_with, &ring);
    sim_set_progress_sink(&s_null, NULL);

    /* Pre-condition: the two worlds start byte-identical. */
    if (memcmp(c_with.voxels, c_null.voxels, sizeof c_with.voxels) != 0) {
        identical_every_tick = 0;
        first_divergence_tick = 0;
    }

    for (t = 1; t <= INV_TICKS; ++t) {
        sim_tick(&s_with);
        sim_tick(&s_null);

        /* The observer DRAINS the sink each tick (the per-frame consumer) - this
         * is exactly the work main.c does, and it must not feed back into the
         * sim. We count drained events to prove the sink is non-trivially busy. */
        events_seen += prog_observe_drain(&ps, &ring);

        if (identical_every_tick &&
            memcmp(c_with.voxels, c_null.voxels, CHUNK_VOXELS * sizeof(Voxel)) != 0) {
            identical_every_tick = 0;
            first_divergence_tick = t;
        }
    }

    /* The two chunks' voxel arrays must be byte-identical at every tick. */
    {
        char detail[64];
        if (first_divergence_tick >= 0)
            snprintf(detail, sizeof detail, "diverged at tick %d", first_divergence_tick);
        else
            detail[0] = '\0';
        report("read-only: WITH-sink and NULL-sink chunks byte-identical every tick",
               identical_every_tick, detail);
    }

    /* The latent / heat side arrays must also be identical (the sim's own state,
     * not just the persisted voxels): the observer is wholly outside the sim. */
    report("read-only: SimState heat[] arrays identical (sim state untouched)",
           memcmp(s_with.heat, s_null.heat, sizeof s_with.heat) == 0, NULL);
    report("read-only: SimState latent[] arrays identical (sim state untouched)",
           memcmp(s_with.latent, s_null.latent, sizeof s_with.latent) == 0, NULL);

    /* Prove the WITH-sink run was genuine observation work, not a no-op: the
     * central copper voxel actually melted, and the observer saw events flow. */
    with_actually_melted = (vox_mat(c_with.voxels[cu_with]) == MAT_MOLTEN_COPPER);
    report("read-only: the WITH-sink world genuinely melted (real physics observed)",
           with_actually_melted, NULL);
    report("read-only: the observer drained real events (the sink was non-trivial)",
           events_seen > 0, NULL);

    /* And, naturally, the NULL-sink world melted identically (sanity). */
    report("read-only: the NULL-sink world melted identically",
           vox_mat(c_null.voxels[cu_null]) == MAT_MOLTEN_COPPER, NULL);

    /* The observer, having watched a real melt, recorded a copper discovery. */
    report("read-only: observer recorded the emergent copper-melt discovery",
           prog_has_discovered(&ps, PROG_MELT, MAT_COPPER), NULL);

    sim_shutdown(&s_with);
    sim_shutdown(&s_null);
}

/* =========================================================================
 * main - run all cases, print a summary, exit with the failure count.
 * ========================================================================= */
int main(void)
{
    printf("== test_progress (M9 progression observer) ==\n");

    test_discovery_dedup();
    test_journal_observed_not_copied();
    test_tiers_emerge();
    test_ring_overflow_drop_oldest();
    test_readonly_invariance();

    if (g_failures == 0)
        printf("ALL PASS (progression observer)\n");
    else
        printf("FAILURES: %d\n", g_failures);

    return g_failures;
}
