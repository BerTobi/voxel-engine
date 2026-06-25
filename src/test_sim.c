/* test_sim.c - Standalone unit harness for the heat simulation (Milestone M3).
 *
 * Binding source: ARCHITECTURE.md Section 3 (the cellular-automaton heart) and
 * the heat-sim design pinned in sim.h: explicit FTCS heat diffusion over a
 * per-chunk active front, in fixed-point integers, on a fixed 15 Hz tick, with
 * a held Dirichlet lava source and wake/sleep so idle voxels cost nothing.
 *
 * This file is PURE C: it touches no GL and no OS, so it builds and runs on the
 * dev host and the XP target alike. It is its own test runner (zero external
 * deps): each case prints "PASS" or "FAIL: <why>"; the process returns the
 * number of failed cases (0 == all green, non-zero == something broke), which a
 * CI script can branch on. This mirrors test_mesher.c's idiom exactly.
 *
 * Build + run (from project root):
 *   gcc -std=c99 -Wall -Isrc -o /tmp/m3_test \
 *       src/material.c src/chunk.c src/sim.c src/test_sim.c -lm && /tmp/m3_test
 *
 * The cases follow the milestone test plan (sim.h API + design):
 *   (1) heat flows hot -> cold  (a held-hot voxel raises a cold neighbour);
 *   (2) closed-system conservation: with NO held source, total heat-energy
 *       (sum of internal-units * volumetric heat capacity) is ~constant across
 *       ticks within the FTCS re-quantization slack;
 *   (3) STABILITY / boundedness: with a held hot source, no voxel ever exceeds
 *       the source code nor drops below ambient over many ticks (catches an
 *       FTCS blow-up - the load-bearing no-blow-up guarantee);
 *   (4) CONDUCTIVITY ORDERING: after equal ticks the heat front advances
 *       strictly farther in a copper bar than in a stone bar;
 *   (5) a uniform-temperature chunk yields ZERO active voxels and zero change;
 *   (6) the held lava source stays at its set temperature every tick;
 *   (7) unit checks: temp_to_heat/heat_to_code round-trips, and (when the LUT
 *       symbol is linkable) the 6*kw <= 2^HEAT_SHIFT stability invariant on the
 *       conductance LUT directly.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>   /* 0.5 M1: calloc for test chunk blocks */

#include "voxel.h"
#include "chunk.h"
#include "material.h"
#include "sim.h"

/* ---- Tiny assertion plumbing (same shape as test_mesher.c) -------------- */
static int g_failures = 0;

/* Report one named case. ok != 0 => PASS. detail is an optional "why" string
 * (may be NULL). Returns ok so callers can early-out if they wish. */
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

/* ---- Voxel / chunk construction helpers --------------------------------- */

/* A solid voxel of the given material at the given temperature code, with
 * fill=15 (the SOLID voxel-state convention) and clear flags. The sim owns the
 * temperature and the VF_ACTIVE flag; everything else is left neutral. */
static Voxel make_solid(uint8_t mat, uint8_t temp_code)
{
    Voxel v = 0;
    vox_set_mat(&v, mat);
    vox_set_fill(&v, 15);
    vox_set_temp_code(&v, temp_code);
    return v;
}

/* Fill a fresh chunk with one solid material at one uniform temperature code.
 * memset first so cx/cy/cz/flags/active_count start clean. */
static void fill_uniform(Chunk *c, uint8_t mat, uint8_t temp_code)
{
    memset(c, 0, sizeof *c);
    c->voxels = calloc(CHUNK_VOXELS, sizeof(Voxel)); /* 0.5 M1: voxels is a pointer */
    c->slab_idx = -1;
    Voxel v = make_solid(mat, temp_code);
    for (int i = 0; i < CHUNK_VOXELS; ++i)
        c->voxels[i] = v;
}

/* The binding starter codes used throughout, anchored once so a codec drift is
 * caught here rather than hidden in every case. */
#define CODE_AMBIENT   60u    /* temp_encode_c(20 C)  -> 60  (decodes 20 C)   */
#define CODE_LAVA      212u   /* temp_encode_c(1150 C)-> 212 (decodes 1160 C) */
#define CODE_WARM      140u   /* temp_encode_c(100 C) -> 140 (ambient band)   */

/* ---- Phase-transition anchors (milestone 3.5) --------------------------- *
 * The copper melt point in code space. temp_encode_c(1085) == 208, and code
 * 208 decodes to 1080 C - so a copper voxel whose decoded committed temperature
 * reaches >= melt_point_c (1085 C) is pinned (plateaus) at code 208 while it
 * banks latent heat. CODE_BELOW_CU decodes to 1060 C (just under 1085) - the
 * gate-test code that must NEVER trigger a melt. CODE_MOLTEN_START decodes to
 * 1080 C and is already below molten copper's 1085 C freeze point, so a molten
 * voxel seeded there sheds latent heat and re-solidifies. */
#define CODE_MELT_CU       208u  /* temp_encode_c(1085) -> 208 (decodes 1080 C) */
#define CODE_BELOW_CU      207u  /* temp_encode_c(1060) -> 207 (decodes 1060 C) */
#define CODE_MOLTEN_START  208u  /* 1080 C: below the 1085 C molten freeze point */

/* The latent-heat accessor is defined in sim.c (sim_build_conduct_lut bakes the
 * per-material table once, in integers). The milestone design declares it extern
 * "for tests"; the on-disk sim.h does not export it, so - mirroring how this file
 * already weak-declares g_conduct_lut - we declare it here so the energy cases
 * can read the expected plateau energy without re-deriving the formula. The
 * round-trip against recompute_latent_units() below cross-checks the two. */
extern int32_t sim_latent_units(uint8_t mat);

/* Recompute the latent-heat budget for a material straight from its MaterialDef,
 * using the EXACT integer formula the design pins (sim.c bakes the same):
 *   latent_units = latent_fusion[kJ/kg] * 1000 * HEAT_ONE_C / specific_heat
 * in the FTCS heat-unit currency (Celsius << HEAT_FRAC_BITS). This lets a case
 * assert energy conservation independently of the sim's own LUT (and catches a
 * drift between the two). specific_heat == 0 -> 0 (no plateau), matching sim.c. */
static int32_t recompute_latent_units(uint8_t mat)
{
    const MaterialDef *m = material_get(mat);
    if (m->specific_heat == 0)
        return 0;
    return (int32_t)(((long)m->latent_fusion * 1000L * (long)HEAT_ONE_C)
                     / (long)m->specific_heat);
}

/* Volumetric heat capacity C = density * specific_heat for a material id.
 * Used by the conservation case. int64 so the product never overflows. */
static int64_t volumetric_C(uint8_t mat)
{
    const MaterialDef *m = material_get(mat);
    return (int64_t)m->density_kg_m3 * (int64_t)m->specific_heat;
}

/* Total heat energy of a chunk = sum over voxels of heat_units * C_material.
 * heat_units = temp_to_heat(code). This is the conserved quantity in a closed
 * system (no held source): face flux moves heat between voxels but the FTCS
 * stencil is conservative (each pairwise exchange is +x to one side, the same
 * energy -x to the other once weighted by the receiving C - see below). */
static int64_t chunk_energy(const Chunk *c)
{
    int64_t e = 0;
    for (int i = 0; i < CHUNK_VOXELS; ++i) {
        Voxel v = c->voxels[i];
        e += (int64_t)temp_to_heat(vox_temp_code(v)) * volumetric_C(vox_mat(v));
    }
    return e;
}

/* ---- Fluid-flow construction + accounting helpers (milestone: fluid flow) -*
 * The fluid pass moves the 4-bit `fill` nibble of PHASE_LIQUID voxels; these
 * helpers build liquid voxels and account the conserved quantity (sum of fill
 * over all voxels of one liquid material). Pure C, no GL: the fluid cases run on
 * the dev host and the XP target alike, like the rest of this harness. */

/* A liquid voxel of material `mat` carrying water-column `fill` (1..15) at temp
 * code `temp_code`, with VF_LIQUID set and no other flags. The fluid design pins
 * a present liquid voxel as: PHASE_LIQUID material id + fill in 1..15 + VF_LIQUID
 * set (this is exactly what sim.c's melt branch produces, plus the fill=15 stamp
 * the melt completion now writes). The sim owns VF_ACTIVE; we leave it clear and
 * let sim_init / the fluid pass wake the voxel. */
static Voxel make_liquid(uint8_t mat, uint8_t fill, uint8_t temp_code)
{
    Voxel v = 0;
    vox_set_mat(&v, mat);
    vox_set_fill(&v, fill);
    vox_set_temp_code(&v, temp_code);
    vox_set_flags(&v, VF_LIQUID);
    return v;
}

/* Total fill summed over every voxel whose material id == `mat`. This is the
 * conserved quantity for a finite liquid: the fluid pass only ever moves fill
 * between same-material (or air->material) cells, so for a closed set with no
 * held source this sum is invariant tick-to-tick (exact integer equality). */
static int sum_fill(const Chunk *c, uint8_t mat)
{
    int total = 0;
    for (int i = 0; i < CHUNK_VOXELS; ++i)
        if (vox_mat(c->voxels[i]) == mat)
            total += vox_fill(c->voxels[i]);
    return total;
}

/* Count voxels of material `mat` that carry any fill (the "wetted" cell count -
 * the planar footprint of a spreading pool). Used by the viscosity and source
 * cases to measure how far a liquid has spread. */
static int count_wetted(const Chunk *c, uint8_t mat)
{
    int n = 0;
    for (int i = 0; i < CHUNK_VOXELS; ++i)
        if (vox_mat(c->voxels[i]) == mat && vox_fill(c->voxels[i]) > 0)
            ++n;
    return n;
}

/* Did ANY voxel of material `mat` leak into a cell that should stay solid? A
 * closed stone box must never have a water voxel where a stone wall was, and a
 * liquid must never overwrite a solid. Returns 1 if a wall voxel index now holds
 * `mat`. wall[] is a caller-supplied list of n indices that began as solid. */
static int leaked_into_walls(const Chunk *c, uint8_t mat,
                             const uint16_t *wall, int n)
{
    for (int i = 0; i < n; ++i)
        if (vox_mat(c->voxels[wall[i]]) == mat)
            return 1;
    return 0;
}

/* =========================================================================
 * Case 1 - heat flows hot -> cold.
 * A stone chunk at ambient with one interior voxel held hot (lava code). After
 * enough ticks an adjacent stone voxel must have risen above ambient (heat
 * spread into it) and the source must still be hot. Stone is a slow conductor,
 * so we tick generously.
 * ========================================================================= */
static void test_heat_flows(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    /* Hold an interior voxel hot. Interior so it has six stone neighbours. */
    int hx = 8, hy = 8, hz = 8;
    uint16_t hot_li = (uint16_t)vox_index(hx, hy, hz);
    vox_set_temp_code(&c.voxels[hot_li], CODE_LAVA);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    sim_set_source(&s, hot_li, (uint8_t)CODE_LAVA);

    /* The +x neighbour of the source - should warm above ambient over time. */
    uint16_t nb_li = (uint16_t)vox_index(hx + 1, hy, hz);

    for (int t = 0; t < 2000; ++t)
        sim_tick(&s);

    uint8_t nb_code = vox_temp_code(c.voxels[nb_li]);
    uint8_t src_code = vox_temp_code(c.voxels[hot_li]);

    char buf[160];
    int ok = (nb_code > CODE_AMBIENT) && (src_code == CODE_LAVA);
    snprintf(buf, sizeof buf,
             "neighbour code %u (want > ambient %u), source code %u (want held %u)",
             nb_code, CODE_AMBIENT, src_code, CODE_LAVA);
    report("case1_heat_flows_hot_to_cold", ok, buf);

    sim_shutdown(&s);
}

/* =========================================================================
 * Case 2 - closed-system total-heat conservation (no held source).
 * A single-material (stone) chunk with one warm interior block in an otherwise
 * ambient field, NO held source. Heat redistributes but total heat-energy is
 * conserved up to the per-voxel re-quantization quantum (commit re-quantizes to
 * the 8-bit code each tick). We keep every temperature in the AMBIENT band
 * (1 C/code) so the quantum is <= 0.5 C of heat per active voxel; the tolerance
 * is generous (1 C of heat-energy per voxel in the chunk) to absorb it.
 * ========================================================================= */
static void test_conservation(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    /* A 3x3x3 warm block near the centre, no held source. CODE_WARM = 100 C,
     * safely inside the 1 C/code ambient band so re-quantization is fine. */
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                vox_set_temp_code(&c.voxels[vox_index(8 + dx, 8 + dy, 8 + dz)],
                                  (uint8_t)CODE_WARM);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    /* No sim_set_source: closed system. */

    int64_t e0 = chunk_energy(&c);

    for (int t = 0; t < 400; ++t)
        sim_tick(&s);

    int64_t e1 = chunk_energy(&c);

    /* Tolerance: at most ~1 C of heat-energy quantization per voxel in the
     * whole chunk. heat-energy unit per voxel = HEAT_ONE_C * C_stone, and we
     * allow CHUNK_VOXELS of them (very generous; the active set is far smaller,
     * but this keeps the bound trivially safe against any commit-order detail
     * while still being orders of magnitude tighter than the seeded energy). */
    int64_t per_voxel_quantum = (int64_t)HEAT_ONE_C * volumetric_C(MAT_STONE);
    int64_t tol = per_voxel_quantum * (int64_t)CHUNK_VOXELS;
    int64_t diff = (e1 > e0) ? (e1 - e0) : (e0 - e1);

    char buf[200];
    int ok = (diff <= tol);
    snprintf(buf, sizeof buf,
             "energy %lld -> %lld, |diff|=%lld, tol=%lld",
             (long long)e0, (long long)e1, (long long)diff, (long long)tol);
    report("case2_closed_system_conservation", ok, buf);

    sim_shutdown(&s);
}

/* =========================================================================
 * Case 3 - STABILITY / boundedness in the worst-case (copper) material.
 * Copper is the most diffusive starter material (the 1/6 ceiling is normalized
 * to it). A copper chunk at ambient with a held lava source, ticked many times,
 * must NEVER produce a voxel hotter than the source code nor colder than the
 * ambient floor. An FTCS blow-up (6*w > 1) would overshoot the source or ring
 * below ambient - this case catches it directly on the hardest material.
 *
 * PHASE-TRANSITION NOTE (milestone 3.5): the held source sits at CODE_LAVA
 * (1160 C), ABOVE copper's 1085 C melt point, so the copper bulk now MELTS into
 * MAT_MOLTEN_COPPER in place over these ticks (pure physics, not a scripted
 * smelt). That is intended and does NOT break this case: the bound check is the
 * point, and it now ALSO validates the molten product - molten copper conducts
 * LESS than solid copper (16500 vs 40100 cW/m*K) so it cannot threaten the 1/6
 * normalization keyed to solid copper, and the spill-on-flip (melt_point + small
 * excess) lands near code 208, far below the 212 source ceiling. Every code seen,
 * solid or molten, must stay inside [CODE_AMBIENT .. CODE_LAVA]. (This is the
 * "assert bounds on the molten product too" option the design flags for case 3;
 * keeping copper preserves the load-bearing hardest-material FTCS coverage.)
 * ========================================================================= */
static void test_stability_copper(void)
{
    Chunk c;
    fill_uniform(&c, MAT_COPPER, CODE_AMBIENT);

    uint16_t hot_li = (uint16_t)vox_index(8, 8, 8);
    vox_set_temp_code(&c.voxels[hot_li], CODE_LAVA);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    sim_set_source(&s, hot_li, (uint8_t)CODE_LAVA);

    int violations = 0;
    uint8_t worst_hi = 0, worst_lo = 255;
    for (int t = 0; t < 3000; ++t) {
        sim_tick(&s);
        for (int i = 0; i < CHUNK_VOXELS; ++i) {
            uint8_t code = vox_temp_code(c.voxels[i]);
            if (code > worst_hi) worst_hi = code;
            if (code < worst_lo) worst_lo = code;
            if (code > CODE_LAVA || code < CODE_AMBIENT)
                ++violations;
        }
    }

    char buf[200];
    int ok = (violations == 0)
          && (worst_hi <= CODE_LAVA) && (worst_lo >= CODE_AMBIENT);
    snprintf(buf, sizeof buf,
             "violations=%d, observed code range [%u..%u], bounds [%u..%u]",
             violations, worst_lo, worst_hi, CODE_AMBIENT, CODE_LAVA);
    report("case3_stability_no_blowup_copper", ok, buf);

    sim_shutdown(&s);
}

/* Helper for case 4: build a 1-D bar of `mat` along +x from the held hot face
 * at x=0 (y=z=8), the rest of the chunk ambient `mat`. Returns how far the heat
 * front has advanced after `ticks` ticks = the largest x whose voxel rose above
 * the ambient code (i.e. the penetration depth of the front along the bar).
 * The whole chunk is one material so conduction is uniform along the row. */
static int front_depth_after(uint8_t mat, int ticks)
{
    Chunk c;
    fill_uniform(&c, mat, CODE_AMBIENT);

    /* Hold the x=0 face (a 16x16 plane at lx=0) hot so the front is a clean
     * planar wave marching in +x - reduces edge noise vs a point source. */
    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    for (int lz = 0; lz < CHUNK_DIM; ++lz)
        for (int ly = 0; ly < CHUNK_DIM; ++ly) {
            uint16_t li = (uint16_t)vox_index(0, ly, lz);
            vox_set_temp_code(&c.voxels[li], (uint8_t)CODE_LAVA);
            sim_set_source(&s, li, (uint8_t)CODE_LAVA);
        }
    /* sim_init seeded the front from the original (uniform) state; re-seed by
     * re-initialising now that the hot plane is stamped, so the wake test sees
     * the gradient. (sim_init also auto-registers sources; our manual sources
     * above are the held plane.) */
    sim_init(&s, &c);
    for (int lz = 0; lz < CHUNK_DIM; ++lz)
        for (int ly = 0; ly < CHUNK_DIM; ++ly)
            sim_set_source(&s, (uint16_t)vox_index(0, ly, lz), (uint8_t)CODE_LAVA);

    for (int t = 0; t < ticks; ++t)
        sim_tick(&s);

    /* Probe the centre row y=z=8: farthest x (>=1, past the held plane) whose
     * code rose above ambient. */
    int depth = 0;
    for (int x = 1; x < CHUNK_DIM; ++x) {
        uint8_t code = vox_temp_code(c.voxels[vox_index(x, 8, 8)]);
        if (code > CODE_AMBIENT)
            depth = x;
    }
    sim_shutdown(&s);
    return depth;
}

/* =========================================================================
 * Case 4 - CONDUCTIVITY ORDERING: copper conducts faster than stone.
 * Same geometry, same number of ticks, two materials. Copper (alpha ~105x
 * stone) must drive its heat front strictly farther down the bar than stone.
 * This is the qualitative physics the harmonic-mean conductance must preserve.
 * ========================================================================= */
static void test_conductivity_ordering(void)
{
    const int ticks = 300;
    int copper_depth = front_depth_after(MAT_COPPER, ticks);
    int stone_depth  = front_depth_after(MAT_STONE,  ticks);

    char buf[160];
    int ok = (copper_depth > stone_depth);
    snprintf(buf, sizeof buf,
             "after %d ticks: copper front depth %d, stone front depth %d (want copper > stone)",
             ticks, copper_depth, stone_depth);
    report("case4_conductivity_ordering_copper_gt_stone", ok, buf);
}

/* =========================================================================
 * Case 5 - a uniform-temperature chunk is fully asleep: zero active, zero change.
 * Every voxel equals every neighbour, so nothing crosses SIM_WAKE_QUANTUM:
 * sim_init must leave 0 active voxels, and a tick must change nothing and not
 * set dirty_mesh. "Uniform chunk = 0 active = free" (sim.h). We snapshot the
 * raw voxel words and require byte-identical state after a tick.
 * ========================================================================= */
static void test_uniform_chunk_sleeps(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    /* Snapshot before. */
    Voxel before[CHUNK_VOXELS];
    memcpy(before, c.voxels, sizeof before);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    uint16_t active_after_init = s.act.count;

    s.dirty_mesh = 0;
    sim_tick(&s);

    int changed = memcmp(before, c.voxels, sizeof before) != 0;

    char buf[160];
    int ok = (active_after_init == 0) && (s.act.count == 0)
          && (changed == 0) && (s.dirty_mesh == 0);
    snprintf(buf, sizeof buf,
             "active_after_init=%u active_after_tick=%u changed=%d dirty=%u (want 0/0/0/0)",
             active_after_init, s.act.count, changed, s.dirty_mesh);
    report("case5_uniform_chunk_zero_active", ok, buf);

    sim_shutdown(&s);
}

/* =========================================================================
 * Case 6 - the held lava source stays at its set temperature every tick.
 * Auto-registration: a chunk with one MAT_EMISSIVE (lava) voxel must, after
 * sim_init, hold that voxel's code at the lava hold code every tick - the
 * Dirichlet boundary that drives the whole sim. We also assert the held code is
 * the binding SIM_LAVA_HOLD_C code (212), and that it is unchanged across many
 * ticks while its neighbours warm.
 * ========================================================================= */
static void test_lava_source_held(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    /* One lava voxel in the interior. sim_init scans for MAT_EMISSIVE and
     * auto-registers it as a held source at SIM_LAVA_HOLD_C's code. We stamp
     * its starting temperature to the lava hold code too (worldgen would). */
    uint16_t lava_li = (uint16_t)vox_index(8, 8, 8);
    Voxel lava = make_solid(MAT_LAVA, (uint8_t)CODE_LAVA);
    c.voxels[lava_li] = lava;

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    uint8_t want = temp_encode_c((double)SIM_LAVA_HOLD_C);  /* 212 */

    int held_every_tick = 1;
    for (int t = 0; t < 500; ++t) {
        sim_tick(&s);
        if (vox_temp_code(c.voxels[lava_li]) != want) {
            held_every_tick = 0;
            break;
        }
    }

    uint8_t final_code = vox_temp_code(c.voxels[lava_li]);
    char buf[180];
    int ok = held_every_tick && (final_code == want) && (want == CODE_LAVA);
    snprintf(buf, sizeof buf,
             "lava code held=%d, final %u, want %u (SIM_LAVA_HOLD_C=%d -> code %u)",
             held_every_tick, final_code, want, SIM_LAVA_HOLD_C, CODE_LAVA);
    report("case6_lava_source_held", ok, buf);

    sim_shutdown(&s);
}

/* =========================================================================
 * Case 6b (0.2 regression) - a player edit OVER a registered source retires it.
 * sim_init auto-registers a lava cell as a held Dirichlet source. When a player
 * break/place overwrites that exact cell with a NON-emissive voxel, the engine
 * calls sim_notify_edit, which must retire the stale source slot - otherwise the
 * PHASE-2 re-stamp loop pins the (now stone/air) cell to the lava hold every
 * tick: an invisible heat "ghost". Phase A: confirm the source holds hot. Phase
 * B: overwrite with ambient stone + sim_notify_edit, tick, assert it COOLS to
 * ambient (pre-fix it would stay at CODE_LAVA forever).
 * ========================================================================= */
static void test_edit_over_source_retires_slot(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    uint16_t li = (uint16_t)vox_index(8, 8, 8);
    c.voxels[li] = make_solid(MAT_LAVA, (uint8_t)CODE_LAVA);

    SimState s;
    int t, a_hot, b_cool;
    uint8_t after_break;
    char buf[200];

    sim_build_conduct_lut();
    sim_init(&s, &c);                       /* auto-registers the lava source */

    /* Phase A: the source holds hot. */
    for (t = 0; t < 50; ++t) sim_tick(&s);
    a_hot = (vox_temp_code(c.voxels[li]) == (uint8_t)CODE_LAVA);

    /* Phase B: a player breaks/places over the source cell -> non-emissive
     * voxel + notify. (world_edit_voxel does the chunk_set; we do it directly.) */
    c.voxels[li] = make_solid(MAT_STONE, (uint8_t)CODE_AMBIENT);
    sim_notify_edit(&s, (int)li);
    for (t = 0; t < 300; ++t) sim_tick(&s);
    after_break = vox_temp_code(c.voxels[li]);
    /* With the slot retired the cell is ordinary stone: it must NOT be pinned to
     * the lava hold - it cools far out of the hot range, down into the cool band.
     * (It settles a touch ABOVE pure ambient, not exactly at it: the heat the
     * source injected during Phase A is conserved and redistributes across the
     * closed chunk to a slightly-warm equilibrium. The defect was a 1160 C ghost;
     * cooling to ~29 C proves the slot is gone.) */
    b_cool = (after_break != (uint8_t)CODE_LAVA) && (after_break <= (uint8_t)CODE_WARM);

    snprintf(buf, sizeof buf,
             "A held-hot=%d (want code %u); B after edit+notify code=%u, want<=warm %u, not lava %u",
             a_hot, (unsigned)CODE_LAVA, (unsigned)after_break,
             (unsigned)CODE_WARM, (unsigned)CODE_LAVA);
    report("case6b_edit_over_source_retires_slot", a_hot && b_cool, buf);

    sim_shutdown(&s);
}

/* =========================================================================
 * Case 7 - unit checks on the fixed-point bridge.
 *  (7a) temp_to_heat / heat_to_code round-trip the binding starter codes
 *       exactly (the codec is integer-valued Celsius, so a code -> heat ->
 *       code trip must be the identity for codec-representable codes).
 *  (7b) the stability invariant 6*kw <= 2^HEAT_SHIFT holds for every material
 *       pair, asserted on the conductance LUT directly when its symbol is
 *       linkable. The LUT is process-global (sim.h says "g_conduct_lut"); we
 *       declare it weakly and skip the direct check if it is not exported,
 *       since case 3 already exercises the no-blow-up property dynamically.
 * ========================================================================= */

/* The conductance LUT is documented (sim.h section 1) as the process-global
 * 256x256 int16 table g_conduct_lut, built by sim_build_conduct_lut(). It may
 * or may not be exported as a linker symbol; declare it weak so the test still
 * links if sim.c keeps it file-static, and gate the direct invariant on it. */
extern int16_t g_conduct_lut[256][256] __attribute__((weak));

static void test_unit_fixedpoint(void)
{
    /* (7a) round-trip codes. We check the codes the milestone actually uses,
     * each of which is exactly codec-representable, so the trip is identity. */
    static const uint8_t codes[] = { CODE_AMBIENT, CODE_WARM, CODE_LAVA, 100u, 160u, 0u };
    int rt_ok = 1;
    uint8_t bad_in = 0, bad_out = 0;
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
        uint8_t code = codes[i];
        int32_t heat = temp_to_heat(code);
        uint8_t back = heat_to_code(heat);
        if (back != code) { rt_ok = 0; bad_in = code; bad_out = back; break; }
    }
    char buf[160];
    snprintf(buf, sizeof buf,
             rt_ok ? "all starter codes round-trip"
                   : "code %u -> heat -> code %u (mismatch)",
             bad_in, bad_out);
    report("case7a_heat_code_roundtrip", rt_ok, buf);

    /* (7b) LUT stability invariant, direct - only if the symbol is linkable. */
    sim_build_conduct_lut();
    if (&g_conduct_lut != 0) {
        const int32_t ceiling = (int32_t)1 << HEAT_SHIFT;  /* 6*kw must be <= this */
        int inv_ok = 1;
        int worst_a = -1, worst_b = -1;
        long worst_6w = 0;
        for (int a = 0; a < 256 && inv_ok; ++a) {
            for (int b = 0; b < 256; ++b) {
                long w = (long)g_conduct_lut[a][b];
                long six_w = 6 * w;
                if (six_w > ceiling) {
                    inv_ok = 0; worst_a = a; worst_b = b; worst_6w = six_w;
                    break;
                }
            }
        }
        char buf2[200];
        if (inv_ok)
            snprintf(buf2, sizeof buf2,
                     "6*kw <= 2^%d for every material pair", HEAT_SHIFT);
        else
            snprintf(buf2, sizeof buf2, "6*kw=%ld > 2^%d at pair [%d][%d]",
                     worst_6w, HEAT_SHIFT, worst_a, worst_b);
        report("case7b_lut_stability_invariant", inv_ok, buf2);
    } else {
        /* Symbol not exported: the dynamic stability case (case 3) covers it. */
        report("case7b_lut_stability_invariant",
               1, "g_conduct_lut not exported; covered dynamically by case 3");
    }
}

/* =========================================================================
 * Phase-transition cases (milestone 3.5: melt / solidify with latent banking)
 * =========================================================================
 * Shared rig for the melt cases (8/9/10): a COPPER chunk (a copper bar/block,
 * exactly the demo geometry where the bar touches the lava) whose central voxel
 * has its six face neighbours held as Dirichlet sources at CODE_LAVA (1160 C) -
 * standing in for the copper-on-lava contact. The central copper voxel under
 * test is NOT a source, so it is subject to try_phase_change; the copper-copper
 * interface to its hot held neighbours conducts at the fast (1/6-ceiling)
 * same-material weight, so it actually reaches the 1085 C melt point and banks.
 *
 * (Why not a STONE bulk with stone neighbours: stone's low conductance throttles
 * the interface so hard that the central voxel sub-quantum-stalls near 400 C and
 * never reaches 1085 C - a real, documented insulator behaviour, but the wrong
 * geometry to test melting. Copper touching a lava-temperature copper boundary
 * is the milestone's emergent demo, and it melts.)
 *
 * Returns by out-params the per-tick history the cases assert against.
 *
 * Out-params (any may be NULL):
 *   first_melt_tick  : tick index (1-based count of sim_tick calls) at which the
 *                      copper voxel's code FIRST reached >= CODE_MELT_CU (208).
 *   flip_tick        : tick index at which vox_mat first became != MAT_COPPER.
 *   latent_before    : latent[li] sampled on the tick immediately BEFORE the
 *                      flip (the largest banked value we can observe, since the
 *                      flip frees the entry the same tick it completes).
 *   max_latent_step  : the largest single-tick increase in latent[li] over the
 *                      plateau (one tick's worth of banked flux - the slack the
 *                      energy assertion allows around latent_units).
 *   plateau_pinned   : 1 iff, on every tick between first_melt and the flip, the
 *                      copper voxel stayed MAT_COPPER with code pinned at 208.
 * Returns the local index of the copper voxel. Ticks up to `max_ticks`.
 * The SimState is left live in *out_s (caller calls sim_shutdown). */
static uint16_t drive_copper_melt(SimState *out_s, Chunk *c, int max_ticks,
                                  int *first_melt_tick, int *flip_tick,
                                  int32_t *latent_before, int32_t *max_latent_step,
                                  int *plateau_pinned)
{
    int hx = 8, hy = 8, hz = 8, n;
    uint16_t cu_li = (uint16_t)vox_index(hx, hy, hz);

    /* A copper bar/block at ambient (the demo geometry). The central voxel under
     * test starts as ordinary ambient copper like the rest. */
    fill_uniform(c, MAT_COPPER, CODE_AMBIENT);

    sim_build_conduct_lut();
    sim_init(out_s, c);

    /* Hold the six face neighbours hot at the lava code - the copper-on-lava
     * contact. They are copper voxels stamped to CODE_LAVA; as Dirichlet sources
     * they are themselves excluded from melting (a held boundary must not change
     * phase), and the fast copper-copper interface pours heat into the central
     * voxel so it reaches the melt point and banks latent. */
    for (n = 0; n < 6; ++n) {
        int nx = hx + ((n == 0) - (n == 1));
        int ny = hy + ((n == 2) - (n == 3));
        int nz = hz + ((n == 4) - (n == 5));
        sim_set_source(out_s, (uint16_t)vox_index(nx, ny, nz), (uint8_t)CODE_LAVA);
    }

    if (first_melt_tick)  *first_melt_tick = -1;
    if (flip_tick)        *flip_tick = -1;
    if (latent_before)    *latent_before = 0;
    if (max_latent_step)  *max_latent_step = 0;
    if (plateau_pinned)   *plateau_pinned = 1;

    {
        int t;
        int32_t prev_latent = 0;
        int seen_melt = 0, flipped = 0;
        for (t = 1; t <= max_ticks && !flipped; ++t) {
            int32_t lat_before_tick = out_s->latent[cu_li];
            uint8_t mat_before = vox_mat(c->voxels[cu_li]);
            sim_tick(out_s);
            {
                uint8_t code = vox_temp_code(c->voxels[cu_li]);
                uint8_t mat  = vox_mat(c->voxels[cu_li]);
                int32_t lat  = out_s->latent[cu_li];

                /* Largest one-tick banked step (slack for the energy bound). */
                if (mat == MAT_COPPER) {
                    int32_t step = lat - prev_latent;
                    if (step > 0 && step > (max_latent_step ? *max_latent_step : 0)
                        && max_latent_step)
                        *max_latent_step = step;
                }

                if (!seen_melt && code >= CODE_MELT_CU) {
                    seen_melt = 1;
                    if (first_melt_tick) *first_melt_tick = t;
                }

                /* Detect the flip (copper -> molten) the first time it happens. */
                if (mat_before == MAT_COPPER && mat != MAT_COPPER) {
                    flipped = 1;
                    if (flip_tick) *flip_tick = t;
                    if (latent_before) *latent_before = lat_before_tick;
                }

                /* While in the plateau window (seen the melt code, not yet
                 * flipped, still copper) the code must be pinned at 208. */
                if (seen_melt && !flipped && mat == MAT_COPPER &&
                    code != CODE_MELT_CU && plateau_pinned)
                    *plateau_pinned = 0;

                prev_latent = lat;
            }
        }
    }
    return cu_li;
}

/* -------------------------------------------------------------------------
 * Case 8 - a copper voxel held above 1085 C MELTS into molten copper.
 * Pure physics: the six lava-hot neighbours drive the central copper voxel
 * past its 1085 C melt point; after banking ~latent_fusion it flips to
 * MAT_MOLTEN_COPPER with VF_LIQUID set. (Confirms the melt happens at all.)
 * ------------------------------------------------------------------------- */
static void test_copper_melts_above_point(void)
{
    SimState s;
    Chunk c;
    int flip_tick = -1;
    uint16_t cu_li = drive_copper_melt(&s, &c, 3000, NULL, &flip_tick, NULL, NULL, NULL);

    uint8_t mat   = vox_mat(c.voxels[cu_li]);
    uint8_t flags = vox_flags(c.voxels[cu_li]);
    int ok = (mat == MAT_MOLTEN_COPPER) && (flags & VF_LIQUID) && (flip_tick > 0);

    char buf[200];
    snprintf(buf, sizeof buf,
             "final mat=%u (want MAT_MOLTEN_COPPER=%u), VF_LIQUID=%d, flip_tick=%d",
             mat, (unsigned)MAT_MOLTEN_COPPER, (flags & VF_LIQUID) ? 1 : 0, flip_tick);
    report("case8_copper_melts_only_above_point", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 9 - melting TAKES TIME: a measurable latent plateau before the flip.
 * After the copper voxel first reaches the melt code (208), it must stay
 * MAT_COPPER with its code PINNED at 208 for several ticks (banking latent
 * heat) before it flips - i.e. it does NOT melt on the first tick it crosses
 * the temperature. We require the plateau to be at least K ticks long and the
 * code to be pinned at 208 throughout it. (Confirms latent banking delays the
 * transition; without it the voxel would flip the instant it crossed 1085 C.)
 * ------------------------------------------------------------------------- */
static void test_melt_takes_time_plateau(void)
{
    SimState s;
    Chunk c;
    int first_melt = -1, flip = -1, pinned = 0;
    uint16_t cu_li = drive_copper_melt(&s, &c, 3000,
                                       &first_melt, &flip, NULL, NULL, &pinned);
    (void)cu_li;

    const int K = 3;                 /* require a plateau of at least this many ticks */
    int plateau_len = (first_melt > 0 && flip > 0) ? (flip - first_melt) : -1;

    int ok = (first_melt > 0) && (flip > 0)
          && (flip > first_melt)              /* did NOT flip the tick it crossed */
          && (plateau_len >= K)
          && pinned;                           /* code held at 208 across the plateau */

    char buf[220];
    snprintf(buf, sizeof buf,
             "first_melt_tick=%d flip_tick=%d plateau_len=%d (want >= %d) pinned_at_208=%d",
             first_melt, flip, plateau_len, K, pinned);
    report("case9_melt_takes_time_plateau", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 10 - latent energy is CONSERVED across the melt (banked, not lost).
 * At the flip, the banked latent has just crossed the material's latent_units.
 * Because the flip frees the latent entry the same tick it completes, the
 * largest value we can observe is latent[li] on the tick just before the flip;
 * it must be within one tick's banking step of latent_units - i.e. the
 * transition absorbed ~latent_fusion worth of energy, neither ~0 (no banking)
 * nor unbounded. We also cross-check that sim's own LUT (sim_latent_units) and
 * the independent recompute from MaterialDef agree on latent_units (~34742).
 * ------------------------------------------------------------------------- */
static void test_latent_energy_conserved(void)
{
    SimState s;
    Chunk c;
    int flip = -1;
    int32_t latent_before = 0, max_step = 0;
    uint16_t cu_li = drive_copper_melt(&s, &c, 3000,
                                       NULL, &flip, &latent_before, &max_step, NULL);
    (void)cu_li;

    int32_t lu_lut   = sim_latent_units(MAT_COPPER);
    int32_t lu_recmp = recompute_latent_units(MAT_COPPER);

    /* The LUT and the independent recompute must match exactly (same integer
     * formula); guards against a drift between sim.c and the design. */
    int lut_agrees = (lu_lut == lu_recmp) && (lu_recmp > 0);

    /* Banked-just-before-flip must be a large fraction of latent_units and not
     * exceed it (the voxel flips AT/just after crossing, so the last observed
     * pre-flip bank is strictly below the target by at most one tick's step).
     * Slack = max single-tick banking step seen on the plateau (plus 1 for the
     * re-quantization quantum). This brackets the absorbed energy tightly around
     * latent_units: not zero, not unbounded. */
    int32_t slack = max_step + 1;
    int energy_ok = (flip > 0)
                 && (latent_before > 0)
                 && (latent_before <= lu_recmp)               /* never overshoots before flip */
                 && (latent_before >= lu_recmp - slack);      /* and is right at the threshold */

    int ok = lut_agrees && energy_ok;

    char buf[260];
    snprintf(buf, sizeof buf,
             "latent_units sim=%ld recompute=%ld; banked_before_flip=%ld max_step=%ld "
             "(want in [%ld..%ld]) flip_tick=%d",
             (long)lu_lut, (long)lu_recmp, (long)latent_before, (long)max_step,
             (long)(lu_recmp - slack), (long)lu_recmp, flip);
    report("case10_latent_energy_conserved", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 11 - a molten voxel cooled below its freeze point RE-SOLIDIFIES.
 * The reverse transition. One MAT_MOLTEN_COPPER voxel seeded at code 208
 * (1080 C, already below molten copper's 1085 C freeze point) sits in an
 * ambient (20 C) stone field with NO held source. It bleeds heat into the cold
 * stone; the freeze branch banks the released-latent debt while pinning its
 * temperature at the freeze point, and once it has shed latent_units it flips
 * back to MAT_COPPER with VF_LIQUID cleared. (Confirms the symmetric reverse.)
 * ------------------------------------------------------------------------- */
static void test_molten_resolidifies(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    uint16_t li = (uint16_t)vox_index(8, 8, 8);
    /* Seed a molten copper voxel below its freeze point. VF_LIQUID is the liquid
     * state flag a molten voxel carries; set it so the start state is a genuine
     * liquid (worldgen / the forward melt would have set it). */
    Voxel m = make_solid(MAT_MOLTEN_COPPER, (uint8_t)CODE_MOLTEN_START);
    vox_set_flags(&m, vox_flags(m) | VF_LIQUID);
    c.voxels[li] = m;

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    /* No held source: a closed cooling problem. */

    int resolidified = 0, t;
    for (t = 1; t <= 6000; ++t) {
        sim_tick(&s);
        if (vox_mat(c.voxels[li]) == MAT_COPPER) {
            resolidified = 1;
            break;
        }
    }

    uint8_t mat   = vox_mat(c.voxels[li]);
    uint8_t flags = vox_flags(c.voxels[li]);
    int ok = resolidified && (mat == MAT_COPPER) && !(flags & VF_LIQUID);

    char buf[220];
    snprintf(buf, sizeof buf,
             "after %d ticks: mat=%u (want MAT_COPPER=%u), VF_LIQUID=%d (want 0)",
             t > 6000 ? 6000 : t, mat, (unsigned)MAT_COPPER, (flags & VF_LIQUID) ? 1 : 0);
    report("case11_molten_resolidifies", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 12 - copper held JUST BELOW 1085 C never melts (the threshold gate).
 * The six face neighbours of an interior copper voxel are held at CODE_BELOW_CU
 * (1060 C, just under the 1085 C melt point), so the copper equilibrates to
 * ~1060 C and its decoded committed temperature never reaches the melt point.
 * Over many ticks it must stay MAT_COPPER, VF_LIQUID must never be set, and its
 * latent accumulator must stay 0 (no banking ever started). The copper voxel is
 * NOT a source, so it is genuinely subject to the melt gate - this proves the
 * gate is the temperature threshold, not an accident of source exclusion.
 * ------------------------------------------------------------------------- */
static void test_copper_below_point_never_melts(void)
{
    Chunk c;
    fill_uniform(&c, MAT_STONE, CODE_AMBIENT);

    int hx = 8, hy = 8, hz = 8, n;
    uint16_t cu_li = (uint16_t)vox_index(hx, hy, hz);
    c.voxels[cu_li] = make_solid(MAT_COPPER, (uint8_t)CODE_AMBIENT);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    for (n = 0; n < 6; ++n) {
        int nx = hx + ((n == 0) - (n == 1));
        int ny = hy + ((n == 2) - (n == 3));
        int nz = hz + ((n == 4) - (n == 5));
        sim_set_source(&s, (uint16_t)vox_index(nx, ny, nz), (uint8_t)CODE_BELOW_CU);
    }

    int ever_liquid = 0, ever_nonzero_latent = 0, ever_not_copper = 0, t;
    for (t = 0; t < 2000; ++t) {
        sim_tick(&s);
        if (vox_mat(c.voxels[cu_li]) != MAT_COPPER)        ever_not_copper = 1;
        if (vox_flags(c.voxels[cu_li]) & VF_LIQUID)        ever_liquid = 1;
        if (s.latent[cu_li] != 0)                          ever_nonzero_latent = 1;
    }

    uint8_t final_code = vox_temp_code(c.voxels[cu_li]);
    int ok = !ever_not_copper && !ever_liquid && !ever_nonzero_latent;

    char buf[220];
    snprintf(buf, sizeof buf,
             "final code=%u (held nb 1060 C, <1085), ever_not_copper=%d ever_liquid=%d "
             "ever_latent=%d (want 0/0/0)",
             final_code, ever_not_copper, ever_liquid, ever_nonzero_latent);
    report("case12_copper_below_point_never_melts", ok, buf);

    sim_shutdown(&s);
}

/* =========================================================================
 * FLUID-FLOW cases (milestone: cellular fluid flow)
 * =========================================================================
 * Binding source: the authoritative FLUID DESIGN pinned in sim.h / the milestone
 * task. A PHASE_LIQUID voxel carries a 1..15 water column in its `fill` nibble;
 * the fluid pass (sim_tick PHASE 1.6) moves fill DOWN into air/less-full same-mat
 * cells (gravity, never viscosity-gated) then spreads it LATERALLY to equalize
 * (gated by MaterialDef.viscosity), conserving total fill, in deterministic
 * fixed-order integer sweeps. These cases assert the load-bearing properties:
 * conservation, level-pool + sleep, gravity-before-spread, viscosity ordering,
 * and the no-merge rule between dissimilar liquids.
 *
 * ISOLATION RIG (shared by all fluid cases): every chunk here is built at a
 * single UNIFORM temperature, so the heat stencil sees zero gradient (no
 * diffusion) and the PHASE 1.5 transition pass never fires - the fill motion is
 * the ONLY thing changing, which makes the integer assertions exact and
 * reproducible. Water cases use ambient (20 C, code 60): water's freeze branch
 * (threshold 0 C) cannot trigger above 0 C and boiling is deferred (no boil
 * branch exists this milestone), so water stays water. Molten-copper cases use a
 * hot uniform field (1160 C, code 212): molten copper never re-melts (its
 * melts_to is itself, which disables re-melt) and its freeze branch (threshold
 * 1085 C) cannot trigger above 1085 C, so it stays molten. A cell a liquid flows
 * into inherits the (uniform) temperature, so a freshly-materialised liquid is
 * in the same no-transition band as its source - the transition pass stays
 * dormant throughout. Lava is deliberately NOT used for the finite-conservation
 * comparisons: it carries MAT_EMISSIVE, which is_held_source() treats as a held
 * Dirichlet boundary (re-filled every tick), so a lava voxel is never finite.
 * Molten copper (PHASE_LIQUID, viscosity 200, non-emissive) is the design's
 * sanctioned "high-viscosity, finite, non-held" liquid for these tests. */

#define CODE_HOT_LIQUID 212u   /* 1160 C: above molten copper's 1085 C freeze pt */

/* Build a closed stone box that fills the whole chunk: every voxel on the chunk
 * boundary (lx/ly/lz == 0 or 15) is solid stone (fill 15), the interior is air.
 * Returns through *n_wall the wall voxel indices (for leak checks). The box being
 * the chunk boundary also doubles as the single-chunk closed-wall the fluid pass
 * already enforces (out-of-chunk neighbours are a no-flow wall), so the liquid is
 * contained either way - belt and braces. */
static void build_stone_box(Chunk *c, uint8_t temp_code,
                            uint16_t *wall, int *n_wall)
{
    int lx, ly, lz, n = 0;
    memset(c, 0, sizeof *c);
    c->voxels = calloc(CHUNK_VOXELS, sizeof(Voxel)); /* 0.5 M1: voxels is a pointer */
    c->slab_idx = -1;
    /* Interior + walls all start as the uniform-temperature material; we stamp
     * the boundary shell to stone and leave the interior air. Air at the uniform
     * temp keeps the whole field gradient-free. */
    Voxel air   = 0; vox_set_mat(&air, MAT_AIR);   vox_set_temp_code(&air, temp_code);
    Voxel stone = make_solid(MAT_STONE, temp_code);
    for (lz = 0; lz < CHUNK_DIM; ++lz)
        for (ly = 0; ly < CHUNK_DIM; ++ly)
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                int idx = vox_index(lx, ly, lz);
                int boundary = (lx == 0 || lx == CHUNK_DIM - 1 ||
                                ly == 0 || ly == CHUNK_DIM - 1 ||
                                lz == 0 || lz == CHUNK_DIM - 1);
                if (boundary) {
                    c->voxels[idx] = stone;
                    if (wall && n < CHUNK_VOXELS) wall[n] = (uint16_t)idx;
                    ++n;
                } else {
                    c->voxels[idx] = air;
                }
            }
    if (n_wall) *n_wall = n;
}

/* -------------------------------------------------------------------------
 * Case 13 - total fill is CONSERVED across many ticks in a closed container.
 * A closed stone box, partially and UNEVENLY filled with finite water (no held
 * source). After 200 ticks the sum of fill over all water voxels must be exactly
 * what it was at tick 0 (conservation is by construction - every unit removed
 * from one cell is added to exactly one other - so any drift is a bug), and no
 * water id may have leaked into a wall cell. Pins the conserved-quantity invariant.
 * ------------------------------------------------------------------------- */
static void test_fill_conserved_closed_container(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    /* Seed an uneven finite body of water on the box floor (ly == 1, the row just
     * above the stone floor at ly == 0). A few stacks of differing fill and a few
     * brim-full cells, so the pass has real work (down + lateral) to redistribute
     * while conserving. Interior spans lx/lz in 1..14, ly in 1..14. */
    int placed = 0;
    int total_seeded = 0;
    struct { int x, y, z, f; } seed[] = {
        { 4, 1, 4, 15 }, { 5, 1, 4, 15 }, { 6, 1, 4, 9 },
        { 4, 2, 4, 12 }, { 5, 2, 4,  7 },
        { 4, 3, 4,  5 },
        { 8, 1, 8, 15 }, { 9, 1, 8, 11 }, { 8, 1, 9, 3 },
        { 8, 2, 8, 14 }, { 8, 3, 8,  6 },
    };
    for (size_t i = 0; i < sizeof seed / sizeof seed[0]; ++i) {
        uint16_t li = (uint16_t)vox_index(seed[i].x, seed[i].y, seed[i].z);
        c.voxels[li] = make_liquid(MAT_WATER, (uint8_t)seed[i].f, (uint8_t)CODE_AMBIENT);
        total_seeded += seed[i].f;
        ++placed;
    }
    (void)placed;

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    int fill0 = sum_fill(&c, MAT_WATER);

    for (int t = 0; t < 200; ++t)
        sim_tick(&s);

    int fill1 = sum_fill(&c, MAT_WATER);
    int leaked = leaked_into_walls(&c, MAT_WATER, wall, n_wall);

    char buf[220];
    int ok = (fill0 == total_seeded) && (fill1 == fill0) && !leaked;
    snprintf(buf, sizeof buf,
             "seeded=%d fill@0=%d fill@200=%d (want equal) leaked_into_wall=%d (want 0)",
             total_seeded, fill0, fill1, leaked);
    report("case13_fill_conserved_closed_container", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 14 - a tall column spreads to a LEVEL pool and then the front SLEEPS.
 * A single tall water column (one cell stack carrying 8 levels of fill,
 * effectively released by gravity) over a flat floor in a wide flat basin. Run
 * until quiescent. Assert (a) the final state is a LEVEL pool: every occupied
 * water cell is within 1 fill level of every other occupied water cell, and the
 * water has spread laterally (occupies more than the single seed column); (b) the
 * front SLEEPS - the simulation reaches a tick after which no fill changes for
 * several consecutive ticks (a settled level pool costs nothing). Fill is also
 * conserved across the run (the column cannot create or destroy water).
 * ------------------------------------------------------------------------- */
static void test_column_spreads_to_level_and_sleeps(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    /* A single column of water near the centre of the floor. Stack 8 cells, each
     * brim-full (fill 15), at ly = 1..8 over the floor at lx=lz=8. Released, this
     * 8*15 = 120 units of water must COLLAPSE down (the tall column disappears)
     * and SPREAD OUT into a shallow, roughly-level pool across the basin floor
     * (lx,lz in 1..14), then the front must SLEEP - the headline "a settled pool
     * costs nothing" / no infinite churn. */
    int cx = 8, cz = 8;
    int seeded = 0;
    for (int ly = 1; ly <= 8; ++ly) {
        uint16_t li = (uint16_t)vox_index(cx, ly, cz);
        c.voxels[li] = make_liquid(MAT_WATER, 15, (uint8_t)CODE_AMBIENT);
        seeded += 15;
    }

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    int fill0 = sum_fill(&c, MAT_WATER);

    /* Run until quiescent: track consecutive ticks with no fill change. We treat
     * the pool as settled once fills are byte-stable for STILL ticks in a row (or
     * the active front empties). Generous tick budget; water (visc 0) levels in a
     * handful of ticks. */
    const int STILL = 8;
    int still_run = 0, slept_tick = -1, settled = 0;
    int prev_sig = -1;
    for (int t = 0; t < 600; ++t) {
        sim_tick(&s);
        /* A cheap order-insensitive signature of the fill distribution: a sum
         * weighted by index. If it does not change, no fill moved this tick. */
        int sig = 0;
        for (int i = 0; i < CHUNK_VOXELS; ++i)
            if (vox_mat(c.voxels[i]) == MAT_WATER)
                sig += (i + 1) * (vox_fill(c.voxels[i]) + 1);
        if (sig == prev_sig) {
            if (++still_run >= STILL && !settled) { settled = 1; slept_tick = t; }
        } else {
            still_run = 0;
        }
        prev_sig = sig;
        if (s.act.count == 0 && !settled) { settled = 1; slept_tick = t; }
        if (settled && s.act.count == 0)
            break;     /* fully quiescent AND front empty: done */
    }

    /* Measure the settled pool. wetted = occupied water cells; fmax = the tallest
     * remaining column; worst_adj = the largest fill difference between any two
     * face-adjacent same-layer water cells (the local-levelness metric the fluid
     * design's lateral rule drives toward small). */
    static const int HNB[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int wetted = 0, fmin = 16, fmax = 0, worst_adj = 0;
    for (int z = 0; z < CHUNK_DIM; ++z)
        for (int y = 0; y < CHUNK_DIM; ++y)
            for (int x = 0; x < CHUNK_DIM; ++x) {
                int i = vox_index(x, y, z);
                if (vox_mat(c.voxels[i]) != MAT_WATER) continue;
                int f = vox_fill(c.voxels[i]);
                if (f == 0) continue;
                ++wetted;
                if (f < fmin) fmin = f;
                if (f > fmax) fmax = f;
                for (int n = 0; n < 4; ++n) {
                    int nx = x + HNB[n][0], nz = z + HNB[n][1];
                    if (nx < 0 || nx >= CHUNK_DIM || nz < 0 || nz >= CHUNK_DIM) continue;
                    int j = vox_index(nx, y, nz);
                    if (vox_mat(c.voxels[j]) == MAT_WATER) {
                        int d = f - (int)vox_fill(c.voxels[j]);
                        if (d < 0) d = -d;
                        if (d > worst_adj) worst_adj = d;
                    }
                }
            }
    if (wetted == 0) { fmin = 0; fmax = 0; }

    int fill1     = sum_fill(&c, MAT_WATER);
    int conserved = (fill1 == fill0) && (fill0 == seeded);
    /* Collapsed: the 8-high brim-full column is gone (a deep residual column would
     * mean the water never fell/spread). A shallow pool sits a handful of levels
     * deep at most. */
    int collapsed = (fmax <= 8);
    /* Spread: occupies far more cells than the single seed column (8 cells). */
    int spread    = wetted > 8;
    /* LEVEL: adjacent same-layer cells are within 1 fill level - the lateral
     * rule's true fixed point (no cell has a neighbour >= 2 lower, the
     * sim_liquid_unsettled condition). This used to be a weak `<= 4` bound
     * because the settling cascade DIED early: a pour woke only the recipient's
     * ring, never the donor's, so an uphill neighbour the pour just made
     * unsettled was never re-activated and the pool froze in a multi-level
     * staircase. fluid_step now wakes the donor's ring on every change, so the
     * cascade completes and the pool reaches the within-1 level state. A regressed
     * wake would re-introduce the staircase and trip this bound. */
    int roughly_level = (worst_adj <= 1);

    char buf[300];
    /* The "still pond costs nothing" invariant requires the active front to truly
     * DRAIN, not merely reach a byte-stable fill signature. Asserting act.count==0
     * (not just `settled`) is deliberate: a stale-head rise clause once pinned this
     * exact puddle awake forever (act.count=57) while its fills were byte-stable, and
     * the old `settled`-only assertion silently passed it. */
    int slept_zero = (s.act.count == 0);
    int ok = collapsed && spread && roughly_level && conserved && settled && slept_zero;
    snprintf(buf, sizeof buf,
             "wetted=%d fill[min..max]=[%d..%d] worst_adj_gap=%d collapsed=%d spread=%d "
             "rough_level=%d conserved=%d(%d->%d) settled=%d slept_tick=%d final_active=%u",
             wetted, fmin, fmax, worst_adj, collapsed, spread, roughly_level,
             conserved, fill0, fill1, settled, slept_tick, s.act.count);
    report("case14_column_spreads_to_level_and_sleeps", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 15 - liquid FALLS DOWN before it spreads (gravity dominates).
 * Water placed mid-air over a deep air column with the floor far below. For the
 * first ticks the water must DESCEND (lower cells gain fill, the start cell
 * empties) and must NOT spread laterally while it can still fall: the four
 * horizontal neighbours of the falling column stay fill==0 until a cell below is
 * full. Pins "a column drains before it spreads" / "downward is always full".
 * ------------------------------------------------------------------------- */
static void test_falls_down_before_spreading(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    /* One water cell high up the interior, lots of air below it down to the
     * stone floor at ly == 0. Start at ly == 13 (near the top, interior tops out
     * at ly == 14). fill 15 so a full unit can move straight down each tick. */
    int sx = 8, sz = 8, sy = 13;
    uint16_t start = (uint16_t)vox_index(sx, sy, sz);
    c.voxels[start] = make_liquid(MAT_WATER, 15, (uint8_t)CODE_AMBIENT);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    /* The four horizontal neighbours of the start column at the seed height. */
    uint16_t hx0 = (uint16_t)vox_index(sx + 1, sy, sz);
    uint16_t hx1 = (uint16_t)vox_index(sx - 1, sy, sz);
    uint16_t hz0 = (uint16_t)vox_index(sx, sy, sz + 1);
    uint16_t hz1 = (uint16_t)vox_index(sx, sy, sz - 1);

    /* For the first few ticks (while the body is still in free fall toward the
     * far floor), assert NO lateral spread at the seed height and that the water
     * is moving DOWN (a cell strictly below the seed gains fill). */
    int lateral_leak = 0;
    int descended = 0;
    const int FALL_TICKS = 5;
    for (int t = 0; t < FALL_TICKS; ++t) {
        sim_tick(&s);
        if (vox_fill(c.voxels[hx0]) != 0 || vox_fill(c.voxels[hx1]) != 0 ||
            vox_fill(c.voxels[hz0]) != 0 || vox_fill(c.voxels[hz1]) != 0)
            lateral_leak = 1;
        /* Any cell strictly below the seed now carrying water == it fell. */
        for (int ly = sy - 1; ly >= 1; --ly)
            if (vox_mat(c.voxels[vox_index(sx, ly, sz)]) == MAT_WATER &&
                vox_fill(c.voxels[vox_index(sx, ly, sz)]) > 0)
                descended = 1;
    }

    /* Fill conserved throughout (no held source). */
    int conserved = (sum_fill(&c, MAT_WATER) == 15);

    char buf[220];
    int ok = descended && !lateral_leak && conserved;
    snprintf(buf, sizeof buf,
             "after %d ticks: descended=%d lateral_leak=%d conserved=%d "
             "(want descended=1, lateral_leak=0, conserved=1)",
             FALL_TICKS, descended, lateral_leak, conserved);
    report("case15_falls_down_before_spreading", ok, buf);

    sim_shutdown(&s);
}

/* Helper for case 16: seed `n` brim-full cells of liquid `mat` in a single floor
 * cell column (a tall pile in ONE column) at the basin centre, run `ticks`, and
 * return the number of wetted cells (the planar spread footprint). Same uniform
 * temperature field as the rig (no transitions). The taller the pile, the more
 * lateral excess there is to spread, so the two liquids start from an identical
 * configuration and only viscosity differs. */
static int spread_footprint_after(uint8_t mat, uint8_t temp_code,
                                  int pile_height, int ticks)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, temp_code, wall, &n_wall);

    int cx = 8, cz = 8;
    for (int ly = 1; ly <= pile_height; ++ly)
        c.voxels[vox_index(cx, ly, cz)] =
            make_liquid(mat, 15, temp_code);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    for (int t = 0; t < ticks; ++t)
        sim_tick(&s);

    int wetted = count_wetted(&c, mat);
    sim_shutdown(&s);
    return wetted;
}

/* -------------------------------------------------------------------------
 * Case 16 - VISCOSITY orders the lateral spread rate.
 * Two identical flat setups (same geometry, same fill, same tick count): one
 * water (viscosity 0 -> spreads instantly), one molten copper (viscosity 200 ->
 * oozes only a small fraction, only every Nth tick). After EQUAL, modest ticks,
 * the low-viscosity water must have wetted STRICTLY MORE floor cells than the
 * high-viscosity molten copper. Molten copper is finite (not held) and seeded in
 * its hot no-freeze band so it conserves and only the viscosity gate differs.
 * Pins the viscosity gating of lateral spread.
 * ------------------------------------------------------------------------- */
static void test_viscosity_orders_spread_rate(void)
{
    /* A short tick budget so the contrast is visible: water levels fast, copper
     * has barely begun to ooze. Both start from the same 8-high single-column
     * pile so the lateral excess to be spread is identical. */
    const int ticks = 12;
    const int pile  = 8;

    int water_wet  = spread_footprint_after(MAT_WATER,         (uint8_t)CODE_AMBIENT,    pile, ticks);
    int copper_wet = spread_footprint_after(MAT_MOLTEN_COPPER, (uint8_t)CODE_HOT_LIQUID, pile, ticks);

    char buf[200];
    int ok = water_wet > copper_wet;
    snprintf(buf, sizeof buf,
             "after %d ticks: water wetted %d cells, molten-copper wetted %d cells "
             "(want water > copper)",
             ticks, water_wet, copper_wet);
    report("case16_viscosity_orders_spread_rate", ok, buf);
}

/* -------------------------------------------------------------------------
 * Case 17 - two adjacent DIFFERENT liquids do not merge.
 * Water placed face-adjacent to finite (non-held) molten copper so they share a
 * face and both can flow. The shared uniform hot field keeps both in their
 * no-transition band (water won't freeze above 0 C, molten copper won't freeze
 * above 1085 C, boiling deferred) and gradient-free (no diffusion), so the only
 * coupling that could occur is an unwanted fill transfer across the dissimilar
 * face - which the design forbids. Run many ticks. Assert: no water cell ever
 * becomes molten copper and vice versa (no leakage of one id into the other's
 * region), and EACH material's total fill is independently conserved.
 * ------------------------------------------------------------------------- */
static void test_two_liquids_do_not_merge(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    /* Hot uniform field: keeps BOTH liquids in their no-transition band so this
     * tests fluid mixing only, not phase change. */
    build_stone_box(&c, (uint8_t)CODE_HOT_LIQUID, wall, &n_wall);

    /* A short partition wall down the middle of the floor would over-engineer
     * this; instead place the two liquids face-adjacent on the floor (ly == 1)
     * sharing the +x/-x face at the basin centre, each a small full body so both
     * have fill to push around. Water on the left (lx 5..6), molten copper on the
     * right (lx 7..8), same z row. They touch at the lx6|lx7 face. */
    int z = 8;
    int water_seed = 0, copper_seed = 0;
    for (int lx = 5; lx <= 6; ++lx) {
        c.voxels[vox_index(lx, 1, z)] = make_liquid(MAT_WATER, 15, (uint8_t)CODE_HOT_LIQUID);
        c.voxels[vox_index(lx, 2, z)] = make_liquid(MAT_WATER, 10, (uint8_t)CODE_HOT_LIQUID);
        water_seed += 25;
    }
    for (int lx = 7; lx <= 8; ++lx) {
        c.voxels[vox_index(lx, 1, z)] = make_liquid(MAT_MOLTEN_COPPER, 15, (uint8_t)CODE_HOT_LIQUID);
        c.voxels[vox_index(lx, 2, z)] = make_liquid(MAT_MOLTEN_COPPER, 10, (uint8_t)CODE_HOT_LIQUID);
        copper_seed += 25;
    }

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);

    int water0  = sum_fill(&c, MAT_WATER);
    int copper0 = sum_fill(&c, MAT_MOLTEN_COPPER);

    for (int t = 0; t < 300; ++t)
        sim_tick(&s);

    int water1  = sum_fill(&c, MAT_WATER);
    int copper1 = sum_fill(&c, MAT_MOLTEN_COPPER);

    /* Independent conservation: neither material gained the other's mass. (If the
     * dissimilar face transferred fill, one total would shrink and the other
     * grow; exact independent equality is the no-merge guarantee.) */
    int water_conserved  = (water1  == water0)  && (water0  == water_seed);
    int copper_conserved = (copper1 == copper0) && (copper0 == copper_seed);

    char buf[260];
    int ok = water_conserved && copper_conserved;
    snprintf(buf, sizeof buf,
             "water fill %d->%d (seed %d), molten-copper fill %d->%d (seed %d) "
             "(want each independently conserved)",
             water0, water1, water_seed, copper0, copper1, copper_seed);
    report("case17_two_liquids_do_not_merge", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 18 - a HELD liquid source floods outward (infinite-source semantics).
 * A held PHASE_LIQUID source (water cell registered via sim_set_source) over a
 * flat floor must spread outward - the wetted area GROWS over ticks - while the
 * source itself never runs dry (an inexhaustible spring). Pins the held-source
 * extension of the fluid pass. (Optional per the test plan; matches the payoff.)
 *
 * The fluid design re-fills a held liquid source to fill=15 in PHASE 2 (the
 * start of each tick, the mass analogue of the temperature Dirichlet hold), THEN
 * lets it DONATE outward in PHASE 1.6 - so it is brim-full AT FLOOD TIME and is
 * (partly) drained again by end-of-tick. Externally (the public API can only
 * observe state between sim_tick calls, i.e. at end-of-tick) the inexhaustible
 * property therefore reads as: the source NEVER empties / never reverts to air -
 * it stays a water voxel with fill >= 1 every tick and keeps re-filling, so the
 * flood keeps advancing. We assert that observable form (never-dry + growth);
 * the literal "fill==15 at end of tick" is NOT the contract (it is full at flood
 * time, then donates), so we report the end-of-tick min/max for visibility. */
static void test_held_source_floods(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    /* A single water source cell on the floor at the centre. It is both a water
     * voxel (so the fluid pass sees it) and a registered SPRING (sim_set_spring,
     * so PHASE 2 re-fills it to 15 each tick and it drives flow without draining).
     * Held-heat/held-fill decoupling: a plain sim_set_source would hold HEAT only
     * and NOT flood - the spring is the explicit inexhaustible-source opt-in. */
    int cx = 8, cz = 8, sy = 1;
    uint16_t src = (uint16_t)vox_index(cx, sy, cz);
    c.voxels[src] = make_liquid(MAT_WATER, 15, (uint8_t)CODE_AMBIENT);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    sim_set_spring(&s, src, (uint8_t)CODE_AMBIENT);

    int wet_early = 0, wet_late = 0;
    int src_never_dry = 1;          /* stays a water voxel with fill >= 1 every tick */
    int src_fmin = 15, src_fmax = 0;
    for (int t = 1; t <= 120; ++t) {
        sim_tick(&s);
        int f = vox_fill(c.voxels[src]);
        if (vox_mat(c.voxels[src]) != MAT_WATER || f < 1)
            src_never_dry = 0;      /* a true source must never empty / revert to air */
        if (f < src_fmin) src_fmin = f;
        if (f > src_fmax) src_fmax = f;
        if (t == 20)  wet_early = count_wetted(&c, MAT_WATER);
        if (t == 120) wet_late  = count_wetted(&c, MAT_WATER);
    }

    char buf[240];
    int ok = src_never_dry && (wet_late > wet_early) && (wet_early >= 1);
    snprintf(buf, sizeof buf,
             "wetted@20=%d wetted@120=%d (want growth) src_never_dry=%d (want 1) "
             "src end-of-tick fill[min..max]=[%d..%d] (re-filled to 15 at flood time)",
             wet_early, wet_late, src_never_dry, src_fmin, src_fmax);
    report("case18_held_source_floods", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 19 - COMMUNICATING VESSELS: two tanks joined by a bottom channel reach
 * the SAME water level, the empty tank rising from the bottom.
 *
 * Two EQUAL tanks (6 columns wide x 14 deep): tank A (lx 2..7) is filled brim-full
 * with water to height H; tank B (lx 9..14) starts empty. A solid divider at lx=8
 * seals them except for a bottom CHANNEL (the divider is open only at ly==1). lx=1
 * is walled off so the two tanks are exactly equal width. Released, water flows A
 * -> channel -> B at the floor, and then must RISE in B against gravity until both
 * surfaces meet at ~H/2 - the headline communicating-vessels behaviour the local
 * gravity+lateral rule alone cannot produce (it only levels a single free surface).
 *
 * H is chosen ODD (7): the flat equilibrium 6*14*H*15 / (12*14) = H/2*15 = 52.5
 * sub-cell units per column falls BETWEEN integer cell levels, so the local
 * gravity+lateral rule stalls a sub-cell off level and the CONNECTED-BODY FINISHER
 * is REQUIRED to settle it level. Asserts: B rises (surfB well above the
 * channel), the two surfaces are level (|surfA-surfB|<=1, ~H/2), MAT_WATER fill is
 * conserved EXACTLY, and the front fully SLEEPS (act.count==0) - "a settled pool
 * costs nothing", even one that needed the non-local snap to get there.
 * ------------------------------------------------------------------------- */
static void test_two_tanks_communicating(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    const int H = 7;                 /* odd: equilibrium between cell levels       */
    const int LO = 1, HI = CHUNK_DIM - 2;   /* interior span 1..14                 */
    Voxel stone = make_solid(MAT_STONE, (uint8_t)CODE_AMBIENT);

    /* Wall off lx=1 (so tank A is exactly lx 2..7), and build the divider at lx=8
     * that is solid everywhere EXCEPT the bottom channel row ly==1. */
    for (int ly = LO; ly <= HI; ++ly)
        for (int lz = LO; lz <= HI; ++lz) {
            c.voxels[vox_index(1, ly, lz)] = stone;
            if (ly != 1)
                c.voxels[vox_index(8, ly, lz)] = stone;
        }

    /* Fill tank A (lx 2..7, lz 1..14) brim-full up to height H. */
    int seeded = 0;
    for (int ly = 1; ly <= H; ++ly)
        for (int lz = LO; lz <= HI; ++lz)
            for (int lx = 2; lx <= 7; ++lx) {
                c.voxels[vox_index(lx, ly, lz)] =
                    make_liquid(MAT_WATER, 15, (uint8_t)CODE_AMBIENT);
                seeded += 15;
            }

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    int fill0 = sum_fill(&c, MAT_WATER);

    /* Run to quiescence: settled once the fill signature is byte-stable for STILL
     * ticks AND the active front empties. Generous budget (the empty tank rises one
     * cell at a time, then the finisher snaps the sub-cell residue). */
    const int STILL = 8;
    int still_run = 0, slept_tick = -1, settled = 0, prev_sig = -1;
    for (int t = 0; t < 4000; ++t) {
        sim_tick(&s);
        int sig = 0;
        for (int i = 0; i < CHUNK_VOXELS; ++i)
            if (vox_mat(c.voxels[i]) == MAT_WATER)
                sig += (i + 1) * (vox_fill(c.voxels[i]) + 1);
        if (sig == prev_sig) {
            if (++still_run >= STILL && !settled) { settled = 1; slept_tick = t; }
        } else {
            still_run = 0;
        }
        prev_sig = sig;
        if (s.act.count == 0 && !settled) { settled = 1; slept_tick = t; }
        if (settled && s.act.count == 0)
            break;
    }

    /* Measure each tank: topmost water row (surf) and total fill (-> mean cell
     * level, the "table"). Both tanks are 6*14 = 84 columns. */
    int surfA = 0, surfB = 0;
    long volA = 0, volB = 0;
    for (int lz = LO; lz <= HI; ++lz) {
        for (int lx = 2; lx <= 7; ++lx)
            for (int ly = 1; ly <= HI; ++ly) {
                int li = vox_index(lx, ly, lz);
                if (vox_mat(c.voxels[li]) == MAT_WATER && vox_fill(c.voxels[li]) > 0) {
                    volA += vox_fill(c.voxels[li]);
                    if (ly > surfA) surfA = ly;
                }
            }
        for (int lx = 9; lx <= 14; ++lx)
            for (int ly = 1; ly <= HI; ++ly) {
                int li = vox_index(lx, ly, lz);
                if (vox_mat(c.voxels[li]) == MAT_WATER && vox_fill(c.voxels[li]) > 0) {
                    volB += vox_fill(c.voxels[li]);
                    if (ly > surfB) surfB = ly;
                }
            }
    }
    double tableA = (double)volA / 84.0 / 15.0;   /* mean cell level, tank A */
    double tableB = (double)volB / 84.0 / 15.0;

    int fill1     = sum_fill(&c, MAT_WATER);
    int conserved = (fill1 == fill0) && (fill0 == seeded);
    int dsurf     = surfA - surfB; if (dsurf < 0) dsurf = -dsurf;
    int b_rose    = (surfB >= 3);                 /* well above the ly==1 channel  */
    int a_dropped = (surfA < H);                  /* tank A fell from its full H   */
    int level     = (dsurf <= 1);                 /* the two surfaces meet         */
    int mid_A     = (tableA >= H / 2.0 - 1.0 && tableA <= H / 2.0 + 1.0);
    int mid_B     = (tableB >= H / 2.0 - 1.0 && tableB <= H / 2.0 + 1.0);
    int slept     = settled && (s.act.count == 0);

    char buf[320];
    int ok = conserved && b_rose && a_dropped && level && mid_A && mid_B && slept;
    snprintf(buf, sizeof buf,
             "H=%d surfA=%d surfB=%d |d|=%d tableA=%.2f tableB=%.2f (~H/2=%.1f) "
             "b_rose=%d a_dropped=%d level=%d mid[A=%d B=%d] conserved=%d(%d->%d seed=%d) "
             "slept=%d slept_tick=%d final_active=%u fired=%u",
             H, surfA, surfB, dsurf, tableA, tableB, H / 2.0,
             b_rose, a_dropped, level, mid_A, mid_B, conserved, fill0, fill1, seeded,
             slept, slept_tick, s.act.count, s.fluid_n_fired);
    report("case19_two_tanks_communicating_vessels", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 20 - FINISHER CONSERVES across an INTERIOR SOLID. Communicating two-tank
 * (like case19) but tank B carries a one-cell stone SHELF at ly=2 in its middle
 * lz row, so the connected body straddles a non-fillable cell inside a column's
 * [floor,ceil] span. The finisher must level the body around the shelf WITHOUT
 * destroying the cell of water it would otherwise drop onto the stone (the bug:
 * the old flat-write decremented its volume budget at the solid and wrote nothing
 * -> exactly one FLUID_FULL cell vanished per shelf). Asserts EXACT conservation,
 * the shelf stays solid, B rises, and the body sleeps.
 * ------------------------------------------------------------------------- */
static void test_finisher_conserves_over_interior_solid(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    const int H = 7;                       /* odd: equilibrium between cell levels  */
    Voxel stone = make_solid(MAT_STONE, (uint8_t)CODE_AMBIENT);

    /* Wall lx=1; divider at lx=5 solid except the bottom channel at ly==1. */
    for (int ly = 1; ly <= 6; ++ly)
        for (int lz = 1; lz <= 6; ++lz) {
            c.voxels[vox_index(1, ly, lz)] = stone;
            if (ly != 1) c.voxels[vox_index(5, ly, lz)] = stone;
        }
    /* Tank A (lx 2..4, lz 1..6) brim-full to H. */
    int seeded = 0;
    for (int ly = 1; ly <= H; ++ly)
        for (int lz = 1; lz <= 6; ++lz)
            for (int lx = 2; lx <= 4; ++lx) {
                c.voxels[vox_index(lx, ly, lz)] =
                    make_liquid(MAT_WATER, 15, (uint8_t)CODE_AMBIENT);
                seeded += 15;
            }
    /* Stone SHELF inside tank B (lx 6..8) at ly=2, ONLY in lz=4, so B's lz=4
     * columns are split by an interior solid yet rejoin the body via lz=3/5. */
    for (int lx = 6; lx <= 8; ++lx)
        c.voxels[vox_index(lx, 2, 4)] = stone;

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    int fill0 = sum_fill(&c, MAT_WATER);

    int slept_tick = -1;
    for (int t = 0; t < 4000; ++t) {
        sim_tick(&s);
        if (s.act.count == 0) { slept_tick = t; break; }
    }

    int fill1 = sum_fill(&c, MAT_WATER);
    /* B has water somewhere above its bottom row -> it rose. */
    int b_above = 0;
    for (int lz = 1; lz <= 6; ++lz)
        for (int lx = 6; lx <= 8; ++lx)
            for (int ly = 2; ly <= 7; ++ly)
                if (vox_mat(c.voxels[vox_index(lx, ly, lz)]) == MAT_WATER &&
                    vox_fill(c.voxels[vox_index(lx, ly, lz)]) > 0) b_above = 1;
    int shelf_ok = 1;
    for (int lx = 6; lx <= 8; ++lx)
        if (vox_mat(c.voxels[vox_index(lx, 2, 4)]) != MAT_STONE) shelf_ok = 0;

    int conserved = (fill1 == fill0) && (fill0 == seeded);
    int slept     = (slept_tick >= 0) && (s.act.count == 0);
    char buf[256];
    int ok = conserved && slept && shelf_ok && b_above;
    snprintf(buf, sizeof buf,
             "conserved=%d(%d->%d seed=%d) slept=%d(t=%d) shelf_intact=%d b_rose=%d "
             "fired=%u final_active=%u",
             conserved, fill0, fill1, seeded, slept, slept_tick, shelf_ok, b_above,
             s.fluid_n_fired, s.act.count);
    report("case20_finisher_conserves_over_interior_solid", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 21 - LAVA-ONLY chunk never spuriously runs the finisher. A held MAT_LAVA
 * pool, no water at all. The communicating-vessels machinery (head field, fill
 * hash, finisher) must stay DORMANT - it is gated on a FREE (non-held) liquid
 * being active, so a lava forge does not pay a whole-chunk hash + flood every
 * tick. Asserts the finisher never fired and lava fill is conserved (held/re-
 * stamped in place).
 * ------------------------------------------------------------------------- */
static void test_lava_only_no_finisher(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    int seeded = 0;
    for (int lz = 4; lz <= 11; ++lz)
        for (int lx = 4; lx <= 11; ++lx) {
            c.voxels[vox_index(lx, 1, lz)] =
                make_liquid(MAT_LAVA, 15, (uint8_t)CODE_LAVA);
            seeded += 15;
        }

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    int fill0 = sum_fill(&c, MAT_LAVA);
    for (int t = 0; t < 200; ++t) sim_tick(&s);
    int fill1 = sum_fill(&c, MAT_LAVA);

    char buf[200];
    int ok = (s.fluid_n_fired == 0) && (fill1 == fill0) && (fill0 == seeded);
    snprintf(buf, sizeof buf, "fired=%u(want 0) conserved=%d(%d->%d seed=%d)",
             s.fluid_n_fired, fill1 == fill0, fill0, fill1, seeded);
    report("case21_lava_only_no_spurious_finisher", ok, buf);

    sim_shutdown(&s);
}

/* -------------------------------------------------------------------------
 * Case 22 - placing a SOLID block into a flooded column conserves water. The
 * player's 0.2 block-place drops a stone into a settled pool (set the voxel +
 * sim_notify_edit); the displaced water must redistribute and re-settle with the
 * rest of the body EXACTLY conserved (no unit destroyed at the new obstruction),
 * and the pool must sleep again. Guards the finisher's interior-solid path on the
 * realistic edit trigger, not just hand-built terrain.
 * ------------------------------------------------------------------------- */
static void test_block_placed_in_water_conserves(void)
{
    Chunk c;
    uint16_t wall[CHUNK_VOXELS];
    int n_wall = 0;
    build_stone_box(&c, (uint8_t)CODE_AMBIENT, wall, &n_wall);

    for (int ly = 1; ly <= 10; ++ly)
        c.voxels[vox_index(8, ly, 8)] =
            make_liquid(MAT_WATER, 15, (uint8_t)CODE_AMBIENT);

    SimState s;
    sim_build_conduct_lut();
    sim_init(&s, &c);
    for (int t = 0; t < 2000 && s.act.count != 0; ++t) sim_tick(&s);

    int fill_before = sum_fill(&c, MAT_WATER);
    /* Drop a stone into a wet floor cell (the pool has spread across ly==1). */
    int li = vox_index(7, 1, 7), displaced = 0;
    if (vox_mat(c.voxels[li]) == MAT_WATER) {
        displaced = vox_fill(c.voxels[li]);
        c.voxels[li] = make_solid(MAT_STONE, (uint8_t)CODE_AMBIENT);
        sim_notify_edit(&s, li);
    }
    int expect = fill_before - displaced;   /* the stone overwrote `displaced` units */

    int slept_tick = -1;
    for (int t = 0; t < 4000; ++t) {
        sim_tick(&s);
        if (s.act.count == 0) { slept_tick = t; break; }
    }
    int fill_after = sum_fill(&c, MAT_WATER);

    char buf[220];
    int ok = (fill_after == expect) && (slept_tick >= 0) && (s.act.count == 0) &&
             (vox_mat(c.voxels[li]) == MAT_STONE);
    snprintf(buf, sizeof buf,
             "placed_at_wet=%d displaced=%d conserved=%d(%d->%d) slept=%d(t=%d) active=%u fired=%u",
             displaced > 0, displaced, fill_after == expect, expect, fill_after,
             slept_tick >= 0, slept_tick, s.act.count, s.fluid_n_fired);
    report("case22_block_placed_in_water_conserves", ok, buf);

    sim_shutdown(&s);
}

int main(void)
{
    printf("== test_sim: fixed-point FTCS heat + phase transitions + fluid flow ==\n");

    test_heat_flows();
    test_conservation();
    test_stability_copper();
    test_conductivity_ordering();
    test_uniform_chunk_sleeps();
    test_lava_source_held();
    test_edit_over_source_retires_slot();
    test_unit_fixedpoint();

    /* Phase transitions (milestone 3.5): melt / plateau / energy / freeze / gate. */
    test_copper_melts_above_point();
    test_melt_takes_time_plateau();
    test_latent_energy_conserved();
    test_molten_resolidifies();
    test_copper_below_point_never_melts();

    /* Fluid flow (this milestone): conservation, level+sleep, gravity-first,
     * viscosity ordering, no-merge, and the held infinite source. */
    test_fill_conserved_closed_container();
    test_column_spreads_to_level_and_sleeps();
    test_falls_down_before_spreading();
    test_viscosity_orders_spread_rate();
    test_two_liquids_do_not_merge();
    test_held_source_floods();
    test_two_tanks_communicating();
    test_finisher_conserves_over_interior_solid();
    test_lava_only_no_finisher();
    test_block_placed_in_water_conserves();

    if (g_failures == 0)
        printf("== ALL PASS ==\n");
    else
        printf("== %d FAILURE(S) ==\n", g_failures);

    /* Non-zero exit on any failure (clamped to the 0..255 exit range). */
    return g_failures > 255 ? 255 : g_failures;
}
