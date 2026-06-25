/* sim.c - The cellular-automaton heat simulation: explicit FTCS diffusion on a
 * per-chunk active front, in fixed-point integers, on a fixed 15 Hz tick.
 *
 * Binding source: sim.h (the authoritative contract for this milestone) and
 * ARCHITECTURE.md Section 3 (the CA heart). This implements heat diffusion plus
 * a held Dirichlet heat source plus the active-front wake/sleep machinery, AND
 * the data-driven phase-transition pass (melt / solidify with latent-heat
 * banking, ARCHITECTURE 3.5 / 3.7 / 3.8). Fluid FLOW (molten metal stays in
 * place this milestone), boiling/evaporation, and cross-chunk seams are still
 * DEFERRED: this milestone is single-chunk and treats out-of-chunk neighbours
 * as a closed no-flux boundary, matching light.c / mesher.c.
 *
 * The transition pass is PHASE 1.5 of sim_tick: it runs AFTER the heat commit
 * (PHASE 2, against the freshly-committed temperatures) and BEFORE compaction
 * (PHASE 3), exactly the ARCHITECTURE 3.7 "heat first, then transitions"
 * ordering. It reads MaterialDef thresholds (melt_point_c, melts_to,
 * freezes_to, latent_fusion, specific_heat, phase) ONLY - there is NO switch on
 * material id anywhere in this file (the hardcoded-recipe anti-pattern the game
 * exists to avoid; behaviour lives in the data table). Latent heat is banked in
 * a SimState-side scratch array (s->latent[], ARCHITECTURE 3.8 "latent
 * accumulator side structure, not the voxel word"), never in the voxel word.
 *
 * WHAT THIS PASS OWNS (extended): the sim still owns TEMPERATURE (bits 8..15)
 * and VF_ACTIVE (bit 28). It now ALSO writes the MATERIAL id (bits 0..7) and the
 * VF_LIQUID flag (bit 29) - but ONLY via MaterialDef melts_to / freezes_to
 * during a transition, never light / ao / fill, which still belong to light.c,
 * the mesher, and the (deferred) fluid pass.
 *
 * Pure C, no GL / no OS dependency (mirrors chunk.c / light.c / mesher.c). It
 * compiles standalone with:
 *   gcc -fsyntax-only -std=c99 -Wall -Isrc src/sim.c
 *
 * The hot loop (sim_tick read pass) is INTEGER FIXED-POINT with NO float
 * (ARCHITECTURE 3.6 / Decision Ledger): the x87 path on the Pentium M is too
 * slow. Float appears only in sim_build_conduct_lut(), which runs ONCE at
 * startup to bake the per-material-pair integer weight table - never per tick,
 * never per voxel.
 */

#include "sim.h"
#include "chunk.h"
#include "voxel.h"
#include "material.h"

#include <stddef.h> /* NULL */
#include <math.h>   /* float ONLY in the one-shot LUT build, never the hot loop */

/* =====================================================================
 * Conductance LUT  (sim.h section 1/2 - the 128 KiB int16 material table)
 * =====================================================================
 * g_conduct_lut[a][b] is the precomputed Q HEAT_SHIFT per-material-pair weight
 * folding harmonic-mean interface conductance, the inverse volumetric heat
 * capacity of the RECEIVING voxel a (so it is asymmetric: lut[a][b]!=lut[b][a]),
 * and the 3D stability normalization. 256*256*2 B = 128 KiB resident, built once
 * and const-after-build. */
static int16_t g_conduct_lut[MAX_MATERIALS][MAX_MATERIALS];
static int     g_lut_built = 0;

/* =====================================================================
 * Latent-heat LUT  (ARCHITECTURE 3.5/3.8 - energy banked during a phase change)
 * =====================================================================
 * g_latent_units[m] is the heat energy (in the SAME internal heat-unit currency
 * as the FTCS loop: Celsius << HEAT_FRAC_BITS, 1/64 C) that one voxel of
 * material m must absorb (to melt) or release (to freeze) before it actually
 * changes phase. It is expressed as the equivalent amount of the material's OWN
 * sensible heat, so banking is commensurate with temp_to_heat and conserves
 * energy against the test's accounting:
 *
 *   latent_units = latent_fusion[kJ/kg] * 1000 / specific_heat[J/(kg K)]   ... = degrees C
 *                  scaled by HEAT_ONE_C into heat units.
 *
 * Precomputed ONCE in sim_build_conduct_lut() by integer math (FLOAT-FREE: the
 * numerator latent_fusion*1000*HEAT_ONE_C is < 2.6e7 for every real material, so
 * the long intermediate fits with huge headroom), so the hot loop never divides.
 * specific_heat == 0 (the all-zero headroom slots) yields 0 (no transition). */
static int32_t g_latent_units[MAX_MATERIALS];

/* The binding lava (MAT_EMISSIVE) hold code, computed ONCE in the float-using
 * startup LUT build so the per-tick held-source re-stamp stays float-free (the
 * codec temp_encode_c emits float; the hot loop must not call it - sim.h "no
 * float in the hot loop"). Holds temp_encode_c(SIM_LAVA_HOLD_C) == 212. */
static uint8_t g_lava_hold_code = 0;

/* =====================================================================
 * Progression tier-band codes  (ARCHITECTURE Section 9 - the TEMP_TIER gates)
 * =====================================================================
 * The 4 coarse capability-milestone temperatures (PROG_TIER_C_WARM/HOT/FORGE/
 * FURNACE = 200/500/1000/1500 C) ENCODED to 8-bit voxel temp codes ONCE, at the
 * float-using LUT-build time. The PHASE 2 commit's TEMP_TIER emit hook then
 * compares the committed old/new temp CODES against these precomputed codes, so
 * the hot loop is FLOAT-FREE (no temp_encode_c per tick - sim.h "no float in the
 * hot loop"). Kept in ascending order so a single new_code can cross several
 * bands at once and each is emitted. Built in sim_build_conduct_lut alongside the
 * other side LUTs; before the build they read 0 (no spurious tier crossings, as
 * a code of 0 == -40 C is below every voxel's start). This is a READ-ONLY side
 * output: it is consulted only when s->progress != NULL (see sim_tick PHASE 2). */
static const int PROG_TIER_C[4] = {
    PROG_TIER_C_WARM, PROG_TIER_C_HOT, PROG_TIER_C_FORGE, PROG_TIER_C_FURNACE
};
static uint8_t g_tier_code[4] = { 0, 0, 0, 0 };

/* =====================================================================
 * Viscosity-cadence LUT  (fluid design VISCOSITY - the every-Nth-tick ooze)
 * =====================================================================
 * g_visc_period[v] is the power-of-two tick PERIOD at which a liquid of
 * VISCOSITY value v is allowed to spread LATERALLY (gravity ignores the gate).
 * Derived from viscosity with a pure shift so the fluid hot loop does no divide:
 *     period = clamp(1 << (v >> 6), FLUID_PERIOD_MIN, FLUID_PERIOD_MAX)
 *     v 0..63   -> 1 (acts every tick: water)
 *     v 64..127 -> 2
 *     v 128..191-> 4
 *     v 192..255-> 8 (molten_iron 230, lava 250)
 * Baked once alongside g_latent_units so the hot loop only indexes. A cell may
 * spread laterally this tick iff (tick_index & (period-1)) == 0. Read through
 * sim_visc_period(mat) below (keyed by material id), which the fluid pass and
 * tests use. */
static uint8_t g_visc_period[256];

/* The lateral STEP BUDGET (fill-levels a cell may push sideways in one acting
 * tick), keyed on viscosity per the design's pinned 0->15 / else->1 split. Pure
 * compare; kept as a helper so the rule lives in one place and reads data-driven
 * (viscosity), never a material id. */
static inline int visc_step_budget(uint8_t visc)
{
    return (visc == 0) ? (int)FLUID_STEP_WATER : (int)FLUID_STEP_VISCOUS;
}

/* Public accessor (sim.h): the lateral cadence PERIOD for a material id. Returns
 * FLUID_PERIOD_MIN (1) for a non-PHASE_LIQUID material (it never spreads, so the
 * period is moot) - keeps the contract honest and the tests' assertion simple. */
uint8_t sim_visc_period(uint8_t mat)
{
    const MaterialDef *md = material_get(mat);
    if (md->phase != PHASE_LIQUID)
        return (uint8_t)FLUID_PERIOD_MIN;
    return g_visc_period[md->viscosity];
}

/* Tiny accessor over the precomputed latent table (declared extern in sim.h for
 * tests, which use it to compute the expected plateau energy without re-deriving
 * the formula). Returns 0 before the LUT is built. */
int32_t sim_latent_units(uint8_t mat)
{
    return g_latent_units[mat];
}

/* The 6 axis-neighbour offsets in (dx,dy,dz), same convention as light.c: the
 * diffusion stencil touches only face neighbours, never diagonals. */
static const int SIM_NEIGH[6][3] = {
    { +1,  0,  0 }, { -1,  0,  0 },
    {  0, +1,  0 }, {  0, -1,  0 },
    {  0,  0, +1 }, {  0,  0, -1 }
};

/* The integer 3D-stability ceiling for a single per-face weight: floor(2^SHIFT
 * / 6). The copper (max-diffusivity) diagonal is clamped to exactly this so
 * 6*w = 6*21845 = 131070 < 131072 = 2^17 strictly (sim.h section 2), and it
 * fits a signed int16 (21845 < 32767). This is the value test 7 asserts the LUT
 * against. */
#define SIM_KW_CEILING ((1 << HEAT_SHIFT) / 6)

/* Representable temperature band in HEAT units (Celsius << HEAT_FRAC_BITS),
 * derived from the binding codec (voxel.h): code 0 == -40 C, code 255 ==
 * T_HOT_BASE_C + (255-T_HOT_CODE)*T_HOT_STEP_C == 2020 C. Used to clamp a
 * committed temperature into range. Built without left-shifting a negative
 * value (which is undefined in C99) by multiplying by HEAT_ONE_C. */
#define HEAT_RANGE_MIN ((int32_t)T_AMB_BASE_C * HEAT_ONE_C)
#define HEAT_RANGE_MAX \
    ((int32_t)(T_HOT_BASE_C + (255 - T_HOT_CODE) * T_HOT_STEP_C) * HEAT_ONE_C)

/* Integer re-quantize: heat units -> 8-bit temperature code, mirroring the
 * binding two-segment round-to-nearest codec in voxel.h (temp_encode_c) but in
 * PURE INTEGER math, so NO float (no x87, no scalar SSE) enters the per-voxel
 * commit in sim_tick - the Decision Ledger / sim.h "no float in the hot loop"
 * rule. (The sim.h heat_to_code is convenient for tests but routes through
 * temp_encode_c((double)c), which emits float; we must not call it per tick.)
 *
 *   First convert heat units -> Celsius with round-to-nearest:
 *       c = round(heat / HEAT_ONE_C)   (symmetric for negative heat)
 *   Then apply the codec's own two-segment round-to-nearest encode in integers.
 * Validated to byte-match heat_to_code(temp_to_heat(code)) for all 256 codes. */
static inline uint8_t heat_to_code_i(int32_t heat)
{
    int32_t c, code;

    /* heat units -> integer Celsius, round-to-nearest (matches sim.h). */
    c = (heat >= 0) ? ( heat + (HEAT_ONE_C / 2)) >> HEAT_FRAC_BITS
                    : -(((-heat) + (HEAT_ONE_C / 2)) >> HEAT_FRAC_BITS);

    if (c <= T_HOT_BASE_C) {
        /* Ambient segment, 1 C / code from -40 C. Round-to-nearest with +half
         * step before the (unit) divide; T_AMB_STEP_C == 1 so the divide is a
         * no-op, but keep the form for parity with the codec. */
        code = (c - T_AMB_BASE_C) * 2 + 1;          /* 2x + half-step, x2 scale */
        code = code / (T_AMB_STEP_C * 2);
    } else {
        /* Industrial segment, 20 C / code from 120 C at code 160. */
        int32_t num = (c - T_HOT_BASE_C) * 2 + T_HOT_STEP_C;  /* x2 for round */
        code = T_HOT_CODE + num / (T_HOT_STEP_C * 2);
    }
    if (code < 0)   code = 0;
    if (code > 255) code = 255;
    return (uint8_t)code;
}

void sim_build_conduct_lut(void)
{
    int a, b;
    double alpha_max, dt;

    if (g_lut_built)
        return;                 /* idempotent: const-after-build */

    /* --- Pick the timestep from the most-diffusive material -----------------
     * Thermal diffusivity alpha = k / (rho * cp), with k in W/(m*K) (the field
     * is cW/(m*K) per material.c, so divide by 100). We normalize so the MOST
     * diffusive material's same-material per-face weight sits at the 1/6 ceiling:
     *     dt = (1/6) / alpha_max
     * with dx = 1 m (one voxel). Computing alpha_max by scan (rather than hard-
     * coding copper) keeps this correct if the material table changes. Only real
     * materials (non-empty name, nonzero density and specific heat) participate;
     * the ~245 all-zero headroom slots have C_a = 0 and would divide by zero. */
    alpha_max = 0.0;
    for (a = 0; a < MAX_MATERIALS; ++a) {
        const MaterialDef *m = &g_materials[a];
        double C = (double)m->density_kg_m3 * (double)m->specific_heat;
        double k = (double)m->thermal_conductivity / 100.0;  /* cW -> W/(m*K) */
        double alpha;
        if (C <= 0.0 || k <= 0.0)
            continue;
        alpha = k / C;
        if (alpha > alpha_max)
            alpha_max = alpha;
    }
    if (alpha_max <= 0.0) {
        /* No usable material (degenerate table); leave the LUT all-zero so the
         * sim becomes a no-op rather than dividing by zero. */
        g_lut_built = 1;
        return;
    }
    dt = (1.0 / 6.0) / alpha_max;

    /* --- Bake every pair ----------------------------------------------------
     *   w[a][b]  = dt * harmonic_mean(k_a, k_b) / C_a          (a == receiver)
     *   kw[a][b] = round( w[a][b] * 2^HEAT_SHIFT )
     * The harmonic mean 2*k_a*k_b/(k_a+k_b) <= min(k_a,k_b) <= k_a guarantees
     * every off-diagonal weight is <= the receiver's same-material weight, which
     * for the max-diffusivity material is the 1/6 ceiling - so the worst-case row
     * sum 6*w stays under 1 and NOTHING can blow up (sim.h section 2). We still
     * clamp defensively to SIM_KW_CEILING so float rounding can never push the
     * stored entry over the int16 / stability bound. */
    for (a = 0; a < MAX_MATERIALS; ++a) {
        const MaterialDef *ma = &g_materials[a];
        double C_a = (double)ma->density_kg_m3 * (double)ma->specific_heat;
        double k_a = (double)ma->thermal_conductivity / 100.0;

        for (b = 0; b < MAX_MATERIALS; ++b) {
            const MaterialDef *mb = &g_materials[b];
            double k_b = (double)mb->thermal_conductivity / 100.0;
            double k_ab, w;
            long   kw;

            /* Empty / non-conducting material on either side -> no flux. C_a==0
             * also means a has no thermal mass, so it cannot receive heat. */
            if (C_a <= 0.0 || k_a <= 0.0 || k_b <= 0.0) {
                g_conduct_lut[a][b] = 0;
                continue;
            }

            k_ab = 2.0 * k_a * k_b / (k_a + k_b);     /* harmonic mean */
            w    = dt * k_ab / C_a;                   /* per-face diffusion weight */
            kw   = lround(w * (double)(1 << HEAT_SHIFT));

            if (kw < 0)
                kw = 0;
            if (kw > SIM_KW_CEILING)
                kw = SIM_KW_CEILING;                  /* hold strictly 6*w < 1 */

            g_conduct_lut[a][b] = (int16_t)kw;
        }
    }

    /* --- Bake the latent-heat table (integer, one-shot, float-free) ---------
     * latent_units = latent_fusion*1000*HEAT_ONE_C / specific_heat, in the same
     * heat-unit currency the FTCS loop banks/spends. Guard specific_heat == 0
     * (all-zero headroom slots) -> 0, i.e. "no latent plateau". The numerator is
     * computed in a long so the *1000*64 scale-up cannot overflow int32 mid-way
     * (max real numerator ~2.6e7, far under LONG_MAX on any C99 target). */
    for (a = 0; a < MAX_MATERIALS; ++a) {
        const MaterialDef *m = &g_materials[a];
        if (m->specific_heat == 0) {
            g_latent_units[a] = 0;
        } else {
            long num = (long)m->latent_fusion * 1000L * (long)HEAT_ONE_C;
            g_latent_units[a] = (int32_t)(num / (long)m->specific_heat);
        }
    }

    /* --- Bake the viscosity cadence table (integer, one-shot, float-free) ---
     * period = 1 << (visc >> 6), clamped to [1,8]. Pure shift/compare; the fluid
     * lateral gate then tests (tick_index & (period-1)) == 0 with no divide. */
    for (a = 0; a < 256; ++a) {
        int sh = a >> 6;                /* 0..3 over the full 0..255 range */
        int period = 1 << sh;           /* 1,2,4,8 */
        if (period < (int)FLUID_PERIOD_MIN) period = (int)FLUID_PERIOD_MIN;
        if (period > (int)FLUID_PERIOD_MAX) period = (int)FLUID_PERIOD_MAX;
        g_visc_period[a] = (uint8_t)period;
    }

    /* Cache the lava hold code now (the only per-tick use of the float codec
     * otherwise), so the held-source re-stamp path is float-free. */
    g_lava_hold_code = temp_encode_c((double)SIM_LAVA_HOLD_C);

    /* Encode the 4 progression tier-milestone temperatures to codes ONCE (the
     * float codec runs here, never in the per-tick TEMP_TIER emit hook). The
     * PHASE 2 commit then compares committed temp codes against these, so the
     * progression side output stays float-free in the hot loop (ARCHITECTURE
     * Section 9 / sim.h). Read-only: consulted only when s->progress != NULL. */
    for (a = 0; a < 4; ++a)
        g_tier_code[a] = temp_encode_c((double)PROG_TIER_C[a]);

    g_lut_built = 1;
}

/* =====================================================================
 * Active-front bitmask helpers  (sim.h section 3 - in_active_mask)
 * ===================================================================== */
static inline int mask_test(const ChunkActive *act, int li)
{
    return (act->in_active_mask[li >> 5] >> (li & 31)) & 1u;
}
static inline void mask_set(ChunkActive *act, int li)
{
    act->in_active_mask[li >> 5] |= (1u << (li & 31));
}
static inline void mask_clear(ChunkActive *act, int li)
{
    act->in_active_mask[li >> 5] &= ~(1u << (li & 31));
}

/* Enqueue local index li onto the active front IF it is not already queued.
 * Sets VF_ACTIVE (the ground truth), the presence mask (enqueue-once test), and
 * appends to the compact list. On overflow (list at cap) the voxel is still
 * flagged VF_ACTIVE so correctness is preserved; overflow just records that a
 * wake could not be queued this tick (graceful in-world slowdown, sim.h 3.7).
 * Returns 1 if newly enqueued, 0 if already present or dropped on overflow. */
static int active_enqueue(SimState *s, int li)
{
    ChunkActive *act = &s->act;
    Voxel *v = &s->chunk->voxels[li];

    /* Always assert the ground-truth flag (cheap, idempotent). */
    vox_set_flags(v, vox_flags(*v) | VF_ACTIVE);

    if (mask_test(act, li))
        return 0;                       /* already on the list */

    if (act->count >= SIM_CHUNK_ACTIVE_MAX) {
        act->overflow = 1;              /* cannot grow; flag stays set, list full */
        return 0;
    }
    mask_set(act, li);
    act->active[act->count++] = (uint16_t)li;
    return 1;
}

/* =====================================================================
 * Source table  (sim.h section 5 - held Dirichlet boundaries)
 * ===================================================================== */
/* Register/update a held source at li, held at hold_code. is_spring=1 ALSO makes
 * a PHASE_LIQUID source re-fill to brim each tick (an inexhaustible spring);
 * is_spring=0 holds HEAT ONLY - the fill is conserved (a hot pool, not a flood).
 * The held-heat / held-fill DECOUPLING lives here: only a spring touches fill. */
static int set_source_impl(SimState *s, uint16_t li, uint8_t hold_code, int is_spring)
{
    int i, free_slot = -1;

    /* Re-register an existing source at the same index (update its hold). */
    for (i = 0; i < SIM_MAX_SOURCES; ++i) {
        if (s->sources[i].active && s->sources[i].li == li) {
            s->sources[i].hold_code = hold_code;
            s->sources[i].is_spring = (uint8_t)is_spring;
            /* A held source is always awake and stamped to its hold now. Sync
             * the authoritative heat[] from the hold code so the precise truth
             * matches the stamped code (the Dirichlet boundary value). */
            vox_set_temp_code(&s->chunk->voxels[li], hold_code);
            s->heat[li] = temp_to_heat(hold_code);
            /* Only a SPRING starts brim-full and is re-filled each tick (PHASE 2)
             * - the mass analogue of the temperature hold. A plain held source
             * holds HEAT ONLY; its fill is conserved so it does not flood. */
            if (is_spring &&
                material_get(vox_mat(s->chunk->voxels[li]))->phase == PHASE_LIQUID)
                vox_set_fill(&s->chunk->voxels[li], FLUID_FULL);
            active_enqueue(s, li);
            return 0;
        }
        if (!s->sources[i].active && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0)
        return 1;                       /* source table full */

    s->sources[free_slot].li        = li;
    s->sources[free_slot].hold_code  = hold_code;
    s->sources[free_slot].active     = 1;
    s->sources[free_slot].is_spring  = (uint8_t)is_spring;
    if (free_slot + 1 > (int)s->n_sources)
        s->n_sources = (uint16_t)(free_slot + 1);

    /* Stamp it to its hold immediately and wake it (a source is always awake so
     * it continuously drives diffusion - sim.h section 5). Sync the authoritative
     * heat[] from the hold code (the precise Dirichlet boundary value). */
    vox_set_temp_code(&s->chunk->voxels[li], hold_code);
    s->heat[li] = temp_to_heat(hold_code);
    if (is_spring &&
        material_get(vox_mat(s->chunk->voxels[li]))->phase == PHASE_LIQUID)
        vox_set_fill(&s->chunk->voxels[li], FLUID_FULL);
    active_enqueue(s, li);
    return 0;
}

/* Public: a held HEAT source (Dirichlet temperature) - holds heat only, so a
 * liquid registered this way keeps its (conserved) fill and does NOT flood. */
int sim_set_source(SimState *s, uint16_t li, uint8_t hold_code)
{
    return set_source_impl(s, li, hold_code, 0);
}

/* Public: a held LIQUID SPRING - heat hold PLUS re-fill to brim each tick, so it
 * floods outward without draining (the inexhaustible-source semantics). */
int sim_set_spring(SimState *s, uint16_t li, uint8_t hold_code)
{
    return set_source_impl(s, li, hold_code, 1);
}

/* 0.5 M3: set the radial-down SIM_NEIGH face for this chunk's binary water flow. */
void sim_set_down_face(SimState *s, int face)
{
    if (s != NULL && face >= 0 && face < 6)
        s->fluid_down = (int8_t)face;
}

/* The progression event-sink setter sim_set_progress_sink() is provided as a
 * `static inline` in sim.h (a single guarded assignment), so no sim.c definition
 * is required. The sim.c-side progression work is the READ-ONLY emit hooks inside
 * try_phase_change (MELT/FREEZE) and sim_tick PHASE 2 (TEMP_TIER), each an
 * `if (s->progress) { build ev; prog_emit(...); }` push - see below. */

/* Is local index li an EXPLICIT (table-registered) held source? Returns the
 * slot index or -1. Used to find a registered source's hold code. */
static int source_index_of(const SimState *s, int li)
{
    int i;
    for (i = 0; i < SIM_MAX_SOURCES; ++i)
        if (s->sources[i].active && s->sources[i].li == li)
            return i;
    return -1;
}

/* Is local index li a HELD Dirichlet boundary - either an explicit registered
 * source OR a MAT_EMISSIVE voxel (lava)? A held voxel is never diffused, never
 * slept, never phase-changed; it is re-stamped to its hold each tick so it
 * continuously drives the sim (sim.h section 5). MAT_EMISSIVE is held DIRECTLY
 * by its material flag rather than via the SIM_MAX_SOURCES table: a real lava
 * POOL can have far more than SIM_MAX_SOURCES voxels (the table is sized for the
 * handful of explicit sources tests register), and the whole pool must act as
 * the hot boundary - otherwise copper resting on a large pool sits on cold,
 * unheld lava and never warms (the table-overflow stall the probe exposes). */
static int is_held_source(const SimState *s, int li)
{
    if (material_get(vox_mat(s->chunk->voxels[li]))->flags & MAT_EMISSIVE)
        return 1;
    return source_index_of(s, li) >= 0;
}

/* The held temperature CODE for a held voxel li: a registered source's own hold
 * code, else (a MAT_EMISSIVE voxel with no table slot) the binding lava hold. */
static uint8_t held_code_of(const SimState *s, int li)
{
    int idx = source_index_of(s, li);
    if (idx >= 0)
        return s->sources[idx].hold_code;
    return g_lava_hold_code;          /* emissive default hold (cached, no float) */
}

/* Is local index li a registered SPRING (re-filled to brim each tick)? Only
 * sources registered via sim_set_spring are; plain held sources AND MAT_EMISSIVE
 * lava (held by flag, no spring slot) are NOT - they hold HEAT only, fill
 * conserved. This is what stops the lava pool flooding the world. */
static int is_spring_source(const SimState *s, int li)
{
    int idx = source_index_of(s, li);
    return (idx >= 0) ? s->sources[idx].is_spring : 0;
}

/* =====================================================================
 * Phase transitions  (ARCHITECTURE 3.5/3.8 - melt / solidify, latent banking)
 * ===================================================================== */

/* Re-wake a voxel and its 6 in-chunk face neighbours. Called when a voxel
 * changes phase: the new material has different conductivity (so the diffusion
 * stencil around it must re-evaluate) and the next solid/liquid in line must
 * re-check its own threshold against the changed neighbour. Out-of-chunk faces
 * are skipped (closed boundary, matching the diffusion stencil). */
static void wake_ring(SimState *s, int li)
{
    int lx = li & 0x0F;
    int ly = (li >> 4) & 0x0F;
    int lz = (li >> 8) & 0x0F;
    int n;

    active_enqueue(s, li);
    for (n = 0; n < 6; ++n) {
        int nx = lx + SIM_NEIGH[n][0];
        int ny = ly + SIM_NEIGH[n][1];
        int nz = lz + SIM_NEIGH[n][2];
        if (nx < 0 || nx >= CHUNK_DIM ||
            ny < 0 || ny >= CHUNK_DIM ||
            nz < 0 || nz >= CHUNK_DIM)
            continue;
        active_enqueue(s, vox_index(nx, ny, nz));
    }
}

/* Try a melt or freeze on local index li against the FRESHLY-COMMITTED voxel
 * temperature. DATA-DRIVEN: reads ONLY MaterialDef fields (melt_point_c,
 * melts_to, freezes_to, latent_fusion via g_latent_units, phase) - no material
 * id is named here. Latent heat is banked in s->latent[li] (0 == no transition
 * in progress). Returns 1 if it changed the material id and/or VF_LIQUID flag
 * (so the caller can flip dirty_mesh), 0 otherwise.
 *
 * Energy accounting (load-bearing, conserves against the test): a voxel at the
 * threshold has its stored temperature CLAMPED to the threshold code, so the
 * temperature PLATEAUS while heat the FTCS loop keeps pushing in is diverted
 * into the bank instead of raising temperature. When the bank reaches
 * g_latent_units[mat], the material flips and the EXCESS over latent is spilled
 * back into the new phase's temperature - so every heat unit either raised temp
 * to the plateau, sits banked, or spilled as excess; nothing is discarded. */
static int try_phase_change(SimState *s, int li)
{
    Voxel *v = &s->chunk->voxels[li];
    uint8_t mat = vox_mat(*v);
    const MaterialDef *md = material_get(mat);
    int32_t latent_units = g_latent_units[mat];
    int32_t threshold_heat, t_heat;

    /* A material with no melt point (-1) has no solidus/liquidus and never
     * transitions (e.g. stone, wood: they burn, they do not melt). */
    if (md->melt_point_c < 0)
        return 0;

    /* MULTIPLY (not <<) by HEAT_ONE_C: safe here today (melt_point_c >= 0 past
     * the guard above) but the multiply matches temp_to_heat's well-defined
     * idiom and removes the latent negative-shift hazard for free. */
    threshold_heat = (int32_t)md->melt_point_c * HEAT_ONE_C;
    /* Read the PRECISE temperature from the authoritative heat[] (full 1/64 C
     * resolution), NOT the re-decoded 8-bit code: banking against the precise
     * value is what makes the plateau and the latent energy accounting exact. */
    t_heat         = s->heat[li];

    /* ---- FREEZE branch: a liquid below its freeze point re-solidifies. -----
     * The freeze point IS the molten material's own melt_point_c (solidus ==
     * liquidus here). Gated purely by data: phase is LIQUID and freezes_to is a
     * distinct solid target. The voxel must SHED latent_units of heat (stay warm
     * at the freeze point) before it can solidify, so we bank the heat DEFICIT
     * below the threshold and clamp temperature UP at the freeze point. */
    if (md->phase == PHASE_LIQUID && md->freezes_to != mat) {
        if (t_heat <= threshold_heat) {
            int32_t deficit = threshold_heat - t_heat;   /* >= 0 */
            int32_t *bank = &s->latent[li];
            *bank += deficit;
            /* Hold temperature pinned UP at the freeze point while shedding -
             * write BOTH the precise heat[] (the plateau truth) and its code. */
            s->heat[li] = threshold_heat;
            vox_set_temp_code(v, heat_to_code_i(threshold_heat));
            if (*bank >= latent_units) {
                int32_t excess = *bank - latent_units;    /* over-shed energy */
                *bank = 0;
                /* ---- Progression OBSERVER emit (READ-ONLY side output) ------
                 * A liquid completed liquid->solid: emit PROG_FREEZE BEFORE the
                 * id swap so `material` is the SOURCE molten id the player was
                 * watching (the journal maps the freeze fact onto md->freezes_to,
                 * the solid it became). O(1), float-free (carries the 8-bit temp
                 * code, not Celsius), no alloc, no callback. GATED on the optional
                 * sink: when s->progress == NULL the event is not even built, so
                 * the sim is byte-identical (ARCHITECTURE Section 9 invariant). */
                if (s->progress) {
                    int lx = li & 0x0F, ly = (li >> 4) & 0x0F, lz = (li >> 8) & 0x0F;
                    ProgressEvent ev;
                    ev.kind               = (uint8_t)PROG_FREEZE;
                    ev.material           = mat;   /* source molten id, pre-swap  */
                    ev.observed_temp_code = vox_temp_code(*v); /* freeze plateau  */
                    ev.tier_code          = 0u;
                    ev.wx = s->chunk->cx * CHUNK_DIM + lx;
                    ev.wy = s->chunk->cy * CHUNK_DIM + ly;
                    ev.wz = s->chunk->cz * CHUNK_DIM + lz;
                    ev.tick = (uint32_t)s->tick_index;
                    prog_emit(s->progress, &ev);
                }
                vox_set_mat(v, md->freezes_to);
                vox_set_flags(v, vox_flags(*v) & ~VF_LIQUID);
                /* A solidified pool cell is a FULL solid: stamp fill=15 (the
                 * SOLID convention). This is the freeze half of the M4<->M5 fill
                 * seam (the fluid design pin): without it a solid frozen out of a
                 * partially-drained liquid would carry a sub-15 fill the solid
                 * world never expects. */
                vox_set_fill(v, FLUID_FULL);
                /* Spill the over-shed energy by setting the new solid BELOW the
                 * freeze point by the excess (symmetric with the melt spill);
                 * write the precise heat[] and derive the storage code. */
                s->heat[li] = threshold_heat - excess;
                vox_set_temp_code(v, heat_to_code_i(s->heat[li]));
                wake_ring(s, li);
                return 1;
            }
            return 0;   /* still banking the plateau; no id change yet */
        }
        /* Back above the freeze point before completing: abandon the deficit so
         * a voxel that re-warms does not carry a stale partial freeze. */
        if (s->latent[li] != 0)
            s->latent[li] = 0;
        return 0;
    }

    /* ---- MELT branch: a material at/above its melt point liquefies. --------
     * Gated purely by data: melts_to is a DISTINCT target (a self-target, as on
     * an already-molten material, disables re-melt; melt_point_c < 0 was already
     * rejected above). Bank the OVER-threshold heat and clamp temperature DOWN
     * to the melt point so it plateaus while soaking latent_units. */
    if (md->melts_to != mat) {
        if (t_heat >= threshold_heat) {
            int32_t over = t_heat - threshold_heat;       /* >= 0 */
            int32_t *bank = &s->latent[li];
            *bank += over;
            /* Hold temperature pinned DOWN at the melt point while soaking -
             * write BOTH the precise heat[] (the plateau truth) and its code. */
            s->heat[li] = threshold_heat;
            vox_set_temp_code(v, heat_to_code_i(threshold_heat));
            if (*bank >= latent_units) {
                int32_t excess = *bank - latent_units;    /* over-soaked energy */
                *bank = 0;
                /* ---- Progression OBSERVER emit (READ-ONLY side output) ------
                 * A solid completed solid->liquid: emit PROG_MELT BEFORE the id
                 * swap so `material` is the SOURCE solid id the player recognises
                 * (e.g. copper, not molten_copper). observed_temp_code is the melt
                 * plateau code the physics committed - the empirical temperature
                 * the journal converges toward (NOT MaterialDef.melt_point_c).
                 * O(1), float-free, no alloc/callback. GATED on the optional sink:
                 * NULL => not even built => byte-identical sim (Section 9). */
                if (s->progress) {
                    int lx = li & 0x0F, ly = (li >> 4) & 0x0F, lz = (li >> 8) & 0x0F;
                    ProgressEvent ev;
                    ev.kind               = (uint8_t)PROG_MELT;
                    ev.material           = mat;   /* source solid id, pre-swap   */
                    ev.observed_temp_code = vox_temp_code(*v); /* melt plateau    */
                    ev.tier_code          = 0u;
                    ev.wx = s->chunk->cx * CHUNK_DIM + lx;
                    ev.wy = s->chunk->cy * CHUNK_DIM + ly;
                    ev.wz = s->chunk->cz * CHUNK_DIM + lz;
                    ev.tick = (uint32_t)s->tick_index;
                    prog_emit(s->progress, &ev);
                }
                vox_set_mat(v, md->melts_to);
                vox_set_flags(v, vox_flags(*v) | VF_LIQUID);
                /* A melted solid was a full cubic metre, so it becomes a FULL
                 * liquid: stamp fill=15. This is THE load-bearing line of the
                 * M4<->M5 seam (fluid design pin): without it the just-melted
                 * voxel would have fill==0 and the PHASE 1.6 fluid pass would
                 * immediately revert it to MAT_AIR (the fill==0 -> air rule) -
                 * a correctness bug. The fill it then flows away is conserved by
                 * the fluid pass. */
                vox_set_fill(v, FLUID_FULL);
                /* Spill the excess into the new molten temperature, ABOVE the
                 * melt point - the energy that arrived after the bank filled;
                 * write the precise heat[] and derive the storage code. */
                s->heat[li] = threshold_heat + excess;
                vox_set_temp_code(v, heat_to_code_i(s->heat[li]));
                wake_ring(s, li);
                return 1;
            }
            return 0;   /* still banking the plateau; no id change yet */
        }
        /* Dropped back below the melt point before completing: abandon the
         * partial soak (a solid that re-cools must not carry a stale bank). */
        if (s->latent[li] != 0)
            s->latent[li] = 0;
        return 0;
    }

    return 0;
}

/* =====================================================================
 * Fluid flow  (fluid design - PHASE 1.6: integer gravity + lateral equalise)
 * =====================================================================
 * An integer, deterministic, in-place, conservative liquid-flow pass over the
 * active front. A liquid voxel's water column is the 4-bit `fill` nibble
 * (vox_fill/vox_set_fill, 1..15 when present); the conserved quantity is the sum
 * of fill over all liquid voxels of a given material. Every transfer is integer
 * fill units and is subtracted from one cell and added to exactly one neighbour,
 * so conservation is by construction. Phase is read from MaterialDef.phase
 * (PHASE_LIQUID), NEVER an id switch - behaviour stays data-driven (material.h).
 *
 * DETERMINISM (fluid design DETERMINISM pin): mass transport is in-place (a true
 * double-buffer would alias mass into one cell and break conservation, per
 * ARCHITECTURE 3.3). Directional bias is killed without a PRNG by:
 *   (1) parity-alternating fixed lateral sweep orders keyed on tick_index, so
 *       neither +axis nor -axis statically wins; and
 *   (2) a per-tick MOVED bitmask (g_moved_mask) cleared at the top of the pass:
 *       a cell that has donated or received fill this tick is skipped when the
 *       sweep later reaches it, so a cell cannot double-move in one tick (the
 *       classic falling-sand aliasing fix). The walk is act->active[] order, the
 *       same fixed/stable order the heat and transition passes use.
 *
 * The moved bitmask is s->moved_mask (SimState-side, sim.h): it is fully cleared
 * at the start of every fluid pass and fully consumed within that pass, so it
 * carries no state between ticks and the run is byte-for-byte reproducible from
 * (world, tick_index). It mirrors the ChunkActive in_active_mask layout. */
static inline int moved_test(const SimState *s, int li)
{
    return (s->moved_mask[li >> 5] >> (li & 31)) & 1u;
}
static inline void moved_set(SimState *s, int li)
{
    s->moved_mask[li >> 5] |= (1u << (li & 31));
}

/* The two parity-alternating lateral visit orders over the 4 horizontal faces.
 * Indices are into SIM_NEIGH: 0=+X 1=-X 4=+Z 5=-Z. Even tick offers +axis first
 * on each of X and Z; odd tick offers -axis first. Fully reproducible from the
 * tick parity (no RNG) - the design's "seed is tick_index" with a deterministic
 * permutation. */

/* ===================================================================== *
 * Communicating vessels: HEAD field + PRESSURE LIFT + connected-body FINISHER
 * (fluid milestone 0.2; ported from the validated 2-D prototype wf_finisher.c,
 * "Approach B"). The local gravity+lateral rule levels a single free surface but
 * cannot raise a column against gravity through a bottom channel. These pieces do.
 * ===================================================================== */

/* Surface level of a single cell in SUB-CELL units measured from the chunk floor:
 * the ly full cells below it are FLUID_FULL each, plus the cell's own fill on top.
 * (Matches the prototype surf_of = y*MAXFILL + fill.) */
static inline int surf_of(const SimState *s, int lx, int ly, int lz)
{
    return ly * (int)FLUID_FULL +
           (int)vox_fill(s->chunk->voxels[vox_index(lx, ly, lz)]);
}

/* A liquid cell whose cell directly above (+Y) is NOT covered by liquid (air,
 * solid lid, or out-of-chunk top): the air/lid-water interface where the head
 * field is SEEDED with the true surface level. Matches the prototype free_surface
 * (water cell, and above is top / wall / fill==0). */
static int free_surface(const SimState *s, int lx, int ly, int lz)
{
    const Voxel *vox = s->chunk->voxels;
    int li = vox_index(lx, ly, lz);
    if (material_get(vox_mat(vox[li]))->phase != PHASE_LIQUID ||
        vox_fill(vox[li]) == 0)
        return 0;
    if (ly + 1 >= CHUNK_DIM)
        return 1;                            /* open at the chunk top */
    {
        int ali = vox_index(lx, ly + 1, lz);
        if (material_get(vox_mat(vox[ali]))->phase == PHASE_LIQUID &&
            vox_fill(vox[ali]) > 0)
            return 0;                        /* covered by liquid above: interior */
        return 1;                            /* air / solid lid above: a surface */
    }
}

/* Scratch for the per-body head flood (file-static, single-chunk). */
static int     g_hbody[CHUNK_VOXELS];   /* BFS queue of body cell indices */
static uint8_t g_hseen[CHUNK_VOXELS];   /* flood visited marks            */

/* Recompute the HEAD field FROM SCRATCH each tick: head[li] = the highest free-
 * surface level (surf_of, in sub-cell units) anywhere in the connected same-
 * material liquid body that li belongs to. A cell whose body has a taller free
 * surface somewhere (e.g. a second tank still below the level of a full first tank
 * joined through a bottom channel) gets a head ABOVE its own column surface, which
 * sim_liquid_unsettled reads to keep that cell AWAKE until the body is levelled.
 * Held sources (lava) are flooded as walls and keep head 0 (they never rise).
 *
 * WHY A FRESH PER-BODY FLOOD, not an incremental relaxation: a relaxed standing
 * field stores last-tick values, so when a draining column lowers its surface the
 * OLD higher value circulates as a "ghost" (re-injected below the age cap) and the
 * head momentarily COLLAPSES to a cell's own seed during the ghost->reseed handoff;
 * the sleep guard latches that one-tick dip and sleeps the body PERMANENTLY un-
 * levelled (measured: head 113->29 in one tick, the whole pool asleep next). A
 * from-scratch flood has no stored state and no ghost, so head exactly tracks the
 * CURRENT surfaces and is rock-stable. O(liquid cells) per tick, deterministic
 * (bodies seeded in index order, fixed neighbour order), reads ONLY fill (never
 * the temperature buffer). Gated by the caller on "free liquid present", so a
 * still pond or a lava-only chunk never pays for it. */
static void head_relax(SimState *s)
{
    const Voxel *vox = s->chunk->voxels;
    int li;
    for (li = 0; li < CHUNK_VOXELS; ++li) { s->head[li] = 0; g_hseen[li] = 0; }
    for (li = 0; li < CHUNK_VOXELS; ++li) {
        uint8_t mat = vox_mat(vox[li]);
        int n = 0, qh = 0, maxsurf = 0, k;
        if (g_hseen[li]) continue;
        if (material_get(mat)->phase != PHASE_LIQUID || vox_fill(vox[li]) == 0)
            continue;
        if (is_held_source(s, li)) continue;     /* held lava: head 0, never rises */
        g_hbody[n++] = li; g_hseen[li] = 1;
        while (qh < n) {
            int i = g_hbody[qh++];
            int lx = i & 0x0F, ly = (i >> 4) & 0x0F, lz = (i >> 8) & 0x0F, nn;
            if (free_surface(s, lx, ly, lz)) {
                int sf = surf_of(s, lx, ly, lz);
                if (sf > maxsurf) maxsurf = sf;
            }
            for (nn = 0; nn < 6; ++nn) {
                int nx = lx + SIM_NEIGH[nn][0];
                int ny = ly + SIM_NEIGH[nn][1];
                int nz = lz + SIM_NEIGH[nn][2];
                int nli;
                if (nx < 0 || nx >= CHUNK_DIM || ny < 0 || ny >= CHUNK_DIM ||
                    nz < 0 || nz >= CHUNK_DIM) continue;
                nli = vox_index(nx, ny, nz);
                if (g_hseen[nli]) continue;
                /* Same-material liquid only (the mat gate already excludes lava and
                 * every other material). No per-neighbour is_held_source scan here:
                 * it cost an O(SIM_MAX_SOURCES) lookup on EVERY neighbour of every
                 * flooded cell, and is unnecessary - a held water spring swept into
                 * the body just contributes its surface to head (harmless; the
                 * FINISHER excludes held sources independently). */
                if (vox_mat(vox[nli]) == mat && vox_fill(vox[nli]) > 0) {
                    g_hseen[nli] = 1; g_hbody[n++] = nli;
                }
            }
        }
        for (k = 0; k < n; ++k) s->head[g_hbody[k]] = maxsurf;
    }
}

/* 0.5 M3: true iff li is a PHASE_LIQUID voxel that can still FLOW under the binary
 * rule - i.e. fluid_step would move it this tick. It MIRRORS fluid_step exactly so
 * the sleep guard and the flow agree on the fixed point: a voxel that can neither
 * fall (radial-down neighbour is air) nor flow-to-descent (a lateral air whose own
 * down is air) is settled and sleeps. Reads ONLY material + neighbour material via
 * s->fluid_down - never the temperature buffer - so the heat-sleep guard may consult
 * it without perturbing heat determinism. Out-of-chunk faces are a closed wall
 * (single-chunk M3; cross-chunk flow is M4). A held lava source never flows but is
 * kept awake by the heat machinery regardless, so it needs no special case here. */
int sim_liquid_unsettled(const SimState *s, uint16_t li)
{
    const Voxel *vox = s->chunk->voxels;
    uint8_t mat = vox_mat(vox[li]);
    int lx, ly, lz, down, down_axis, n;

    if (material_get(mat)->phase != PHASE_LIQUID)
        return 0;
    if (vox_fill(vox[li]) == 0)
        return 1;                       /* drained: the pass must revert it to air */

    lx = li & 0x0F; ly = (li >> 4) & 0x0F; lz = (li >> 8) & 0x0F;
    down = s->fluid_down;
    down_axis = down >> 1;

    /* (a) can it FALL? radial-down neighbour (in-chunk) is air. */
    {
        int nx = lx + SIM_NEIGH[down][0], ny = ly + SIM_NEIGH[down][1], nz = lz + SIM_NEIGH[down][2];
        if (nx >= 0 && nx < CHUNK_DIM && ny >= 0 && ny < CHUNK_DIM && nz >= 0 && nz < CHUNK_DIM &&
            vox_mat(vox[vox_index(nx, ny, nz)]) == MAT_AIR)
            return 1;
    }
    /* (b) can it FLOW-TO-DESCENT? a lateral air neighbour whose own down is air. */
    for (n = 0; n < 6; ++n) {
        int nx, ny, nz, dx, dy, dz;
        if ((n >> 1) == down_axis)
            continue;                   /* lateral faces only */
        nx = lx + SIM_NEIGH[n][0]; ny = ly + SIM_NEIGH[n][1]; nz = lz + SIM_NEIGH[n][2];
        if (nx < 0 || nx >= CHUNK_DIM || ny < 0 || ny >= CHUNK_DIM || nz < 0 || nz >= CHUNK_DIM)
            continue;
        if (vox_mat(vox[vox_index(nx, ny, nz)]) != MAT_AIR)
            continue;
        dx = nx + SIM_NEIGH[down][0]; dy = ny + SIM_NEIGH[down][1]; dz = nz + SIM_NEIGH[down][2];
        if (dx < 0 || dx >= CHUNK_DIM || dy < 0 || dy >= CHUNK_DIM || dz < 0 || dz >= CHUNK_DIM)
            continue;
        if (vox_mat(vox[vox_index(dx, dy, dz)]) == MAT_AIR)
            return 1;
    }
    /* 0.5 M4: with cross-chunk flow enabled, a water voxel on the DOWN-FACE boundary
     * may fall into the chunk below - keep it awake so it keeps offering the
     * cross-move each tick until it crosses. Stay awake ONLY when the neighbour
     * chunk's cell below is actually AIR (a viable cross): a boundary voxel resting
     * over SOLID crust below must be allowed to SLEEP, else its chunk's active set
     * never drains (a livelock that eventually fills the WORLDCA_MAX_XSIMS pool and
     * stalls all cross-chunk flow). neigh[] is Face order, SIM_NEIGH is swapped per
     * axis pair, so the down-neighbour chunk is neigh[down ^ 1] (chunk_vox is sparse-
     * air safe). NULL fluid_xfn (single-chunk / tests) skips this => M3. */
    if (s->fluid_xfn != NULL) {
        int dnx = lx + SIM_NEIGH[down][0];
        int dny = ly + SIM_NEIGH[down][1];
        int dnz = lz + SIM_NEIGH[down][2];
        if (dnx < 0 || dnx >= CHUNK_DIM || dny < 0 || dny >= CHUNK_DIM ||
            dnz < 0 || dnz >= CHUNK_DIM) {
            const Chunk *nc = s->chunk->neigh[down ^ 1];
            if (nc != NULL) {
                int nlx = (dnx + CHUNK_DIM) & (CHUNK_DIM - 1);
                int nly = (dny + CHUNK_DIM) & (CHUNK_DIM - 1);
                int nlz = (dnz + CHUNK_DIM) & (CHUNK_DIM - 1);
                if (vox_mat(chunk_vox(nc, vox_index(nlx, nly, nlz))) == MAT_AIR)
                    return 1;               /* air across the seam: keep retrying */
            }
        }
    }
    return 0;
}

/* Materialise a freshly-occupied AIR cell as the flowing liquid `mat` with the
 * given fill and VF_LIQUID set, matching what the melt branch does for a new
 * molten voxel. heat[] is left as-is (a valid air-cell temperature is harmless
 * and re-seeded on the next sim_init; the fluid pass never touches the temp
 * double-buffer, preserving heat determinism). */
static void fluid_occupy_air(SimState *s, int nli, uint8_t mat, int fill)
{
    Voxel *nv = &s->chunk->voxels[nli];
    vox_set_mat(nv, mat);
    vox_set_fill(nv, (uint8_t)fill);
    vox_set_flags(nv, vox_flags(*nv) | VF_LIQUID);
    s->head[nli] = 0;                   /* fresh surface: head re-seeds next relax */
}

/* Revert a drained liquid voxel (fill reached 0) to MAT_AIR: clear material,
 * fill, and VF_LIQUID. Temperature/heat[] is left as-is (heat of an air cell is
 * harmless and re-seeded on next sim_init - the fluid design "drained" rule).
 * The 6-ring is woken by the caller. */
static void fluid_revert_to_air(SimState *s, int li)
{
    Voxel *v = &s->chunk->voxels[li];
    vox_set_mat(v, MAT_AIR);
    vox_set_fill(v, 0);
    vox_set_flags(v, vox_flags(*v) & ~VF_LIQUID);
    s->head[li] = 0;                    /* air carries no head */
}

/* 0.5 M3: in-chunk neighbour of (lx,ly,lz) across SIM_NEIGH face `face`. Returns
 * 1 + the local index in *out; 0 if the neighbour leaves the chunk (a CLOSED WALL
 * in the single-chunk M3 scope - cross-chunk fluid flow is M4). */
static int fluid_neigh(int lx, int ly, int lz, int face, int *out)
{
    int nx = lx + SIM_NEIGH[face][0];
    int ny = ly + SIM_NEIGH[face][1];
    int nz = lz + SIM_NEIGH[face][2];
    if (nx < 0 || nx > 15 || ny < 0 || ny > 15 || nz < 0 || nz > 15)
        return 0;
    *out = vox_index(nx, ny, nz);
    return 1;
}

/* Move (or, for a spring, DONATE) a whole binary-water voxel from `li` into the AIR
 * cell `dst`: dst becomes water (fill=15, VF_LIQUID); the source empties to air
 * UNLESS it is a spring (which keeps its cell and is re-filled in PHASE 2). Both
 * endpoints are marked moved (so neither re-acts this tick) and their rings woken. */
static void fluid_move_water(SimState *s, int li, int dst, uint8_t mat, int is_spring)
{
    /* CARRY the source's fill so the move CONSERVES exactly (binary water is fill=15,
     * so this is 15 -> 15; carrying also conserves any stray partial-fill voxel). A
     * SPRING instead EMITS a full voxel (fill=15) and keeps its own cell (re-filled
     * in PHASE 2), so it is an inexhaustible source by design. */
    int carry = is_spring ? (int)FLUID_FULL : (int)vox_fill(s->chunk->voxels[li]);
    /* A SPRING emits plain MAT_WATER, never its own id - so a MAT_WATER_SOURCE cell
     * floods water, not an exploding field of sources. A non-spring move carries its
     * own material (the moving body keeps its identity). */
    uint8_t emit = is_spring ? (uint8_t)MAT_WATER : mat;
    fluid_occupy_air(s, dst, emit, carry);
    moved_set(s, dst);
    wake_ring(s, dst);
    moved_set(s, li);
    if (!is_spring) {
        fluid_revert_to_air(s, li);
        wake_ring(s, li);
    }
}

/* One BINARY fluid step on PHASE_LIQUID voxel li (0.5 M3, replacing the partial-fill
 * lateral-equalise model). Water is fill {0,15} and moves as a WHOLE voxel, per the
 * validated 3-D prototype (scratchpad/water_ca_proto.c):
 *   (a) GRAVITY - if the radial-DOWN neighbour (s->fluid_down face) is air, fall.
 *   (b) FLOW-TO-DESCENT - else move into an air LATERAL neighbour (a face not on the
 *       down axis) from which water could then fall (its own down-neighbour is air).
 * Binary fill removes the partial-fill gradient the prototypes proved cannot settle,
 * so this is a terminating occupancy spread: a voxel that can neither fall nor
 * flow-to-descent does NOT move and sleeps (sim_liquid_unsettled mirrors the test
 * exactly). Conserves water (occupy + revert is net-zero); a SPRING donates a voxel
 * without emptying. Single-chunk: out-of-chunk faces are a closed wall (M4 opens
 * them). A non-spring held source (lava, MAT_EMISSIVE) holds HEAT in place and does
 * not flow. Returns 1 if the voxel grid changed. */
static int fluid_step(SimState *s, int li, int is_source, int is_spring)
{
    Voxel *vox = s->chunk->voxels;
    uint8_t mat = vox_mat(vox[li]);
    int lx = li & 0x0F, ly = (li >> 4) & 0x0F, lz = (li >> 8) & 0x0F;
    int down = s->fluid_down;
    int down_axis = down >> 1;
    int dn, oi, n, nlat = 0, rot;
    int lat[4];

    if (vox_fill(vox[li]) == 0)
        return 0;                       /* drained: nothing to move */
    if (is_source && !is_spring)
        return 0;                       /* lava / plain held source: conserve in place */

    /* (a) GRAVITY: fall along the radial-down face. */
    if (fluid_neigh(lx, ly, lz, down, &dn)) {           /* down is IN-CHUNK */
        if (vox_mat(vox[dn]) == MAT_AIR && !moved_test(s, dn)) {
            fluid_move_water(s, li, dn, mat, is_spring);
            return 1;
        }
        /* in-chunk down blocked -> fall through to lateral */
    } else if (s->fluid_xfn != NULL && !is_spring) {    /* 0.5 M4: down is OUT-OF-CHUNK */
        /* Ask the WorldCA whether the chunk below has air at this column: if so it
         * enqueues a deferred ATOMIC cross-move (applied after all chunks commit -
         * materialise the neighbour cell + revert this one, conserving) and we stop
         * (gravity beats lateral). If not viable, fall through to in-chunk lateral.
         * A spring never crosses a seam (it would lose its held cell). Pass the
         * wrapped neighbour-local coords (the out-of-chunk axis folds to 0 or 15). */
        int nlx = (lx + SIM_NEIGH[down][0] + CHUNK_DIM) & (CHUNK_DIM - 1);
        int nly = (ly + SIM_NEIGH[down][1] + CHUNK_DIM) & (CHUNK_DIM - 1);
        int nlz = (lz + SIM_NEIGH[down][2] + CHUNK_DIM) & (CHUNK_DIM - 1);
        /* The callback indexes the Chunk neigh[] array, which is in Face-enum order
         * (-X,+X,-Y,+Y,-Z,+Z) - the SWAP of SIM_NEIGH's (+X,-X,+Y,-Y,+Z,-Z) within
         * each axis pair. So convert with (down ^ 1), exactly as the heat cross-chunk
         * path does (see the n^1 above). The neighbour-local coords are already in
         * SIM_NEIGH[down] terms (correct direction) and are passed explicitly. */
        if (s->fluid_xfn(s->fluid_xfn_user, s, li, down ^ 1, nlx, nly, nlz))
            return 0;                                    /* cross-fall enqueued */
    }

    /* (b) FLOW-TO-DESCENT: into an air lateral neighbour that can itself fall.
     * Laterals = the 4 faces NOT on the down axis; rotate the visit start by tick
     * parity for bias-free determinism. */
    for (n = 0; n < 6; ++n)
        if ((n >> 1) != down_axis)
            lat[nlat++] = n;            /* nlat == 4 */
    rot = (s->tick_index & 1u) ? 2 : 0;
    for (oi = 0; oi < 4; ++oi) {
        int face = lat[(oi + rot) & 3];
        int ln, lnd, nlx, nly, nlz;
        if (!fluid_neigh(lx, ly, lz, face, &ln))      continue;
        if (vox_mat(vox[ln]) != MAT_AIR || moved_test(s, ln)) continue;
        nlx = ln & 0x0F; nly = (ln >> 4) & 0x0F; nlz = (ln >> 8) & 0x0F;
        if (!fluid_neigh(nlx, nly, nlz, down, &lnd))  continue;   /* its down */
        if (vox_mat(vox[lnd]) != MAT_AIR)             continue;   /* can't descend */
        fluid_move_water(s, li, ln, mat, is_spring);
        return 1;
    }
    return 0;                           /* settled this tick (can't fall or flow) */
}

/* ===================================================================== *
 * CONNECTED-BODY FINISHER (Approach B): a bounded, conserving, non-local snap
 * that levels a water body the local rule has left in a sub-cell limit cycle.
 * Fired RARELY (once per body per disturbance, gated by the limit-cycle trigger
 * in sim_tick), then the body is a true fixed point and sleeps. All scratch is
 * file-static (single-chunk this milestone).
 * ===================================================================== */
static int      g_body[CHUNK_VOXELS];      /* BFS queue of body cell indices       */
static int      g_col_lx[CHUNK_VOXELS];    /* per-column lx (<=256 columns used)   */
static int      g_col_lz[CHUNK_VOXELS];    /* per-column lz                        */
static int      g_col_floor[CHUNK_VOXELS]; /* lowest body cell ly in the column    */
static int      g_col_ceil[CHUNK_VOXELS];  /* highest open ly the column may rise  */
static int      g_col_vol[CHUNK_VOXELS];   /* assigned sub-cell volume per column  */
static int      g_col_cap[CHUNK_VOXELS];   /* OPEN-cell capacity per column (units) */
static uint8_t  g_seen[CHUNK_VOXELS];      /* flood-fill visited marks             */

/* Is cell li fillable by body material `mat` during a flat-write: an AIR cell or a
 * (non-held) cell already of `mat`. A solid, a held source (lava / spring), or a
 * DIFFERENT liquid is an OBSTRUCTION the surface must skip WITHOUT consuming any of
 * the body's volume budget. Treating an obstruction as fillable (the original port)
 * silently destroyed a full cell of water per shelf / lava-plug bracketed inside a
 * column's [floor,ceil] span - a conservation break. */
static int cell_fillable(const SimState *s, int li, uint8_t mat)
{
    uint8_t cm = vox_mat(s->chunk->voxels[li]);
    if (is_held_source(s, li)) return 0;
    return cm == MAT_AIR || cm == mat;
}

/* A deterministic FNV-1a hash of the whole chunk's fill nibbles - the signature of
 * the liquid state used to detect a sub-cell limit cycle. Air is 0 and solids are
 * a constant 15, so the hash tracks exactly the moving water distribution; an
 * air<->water materialisation flips a 0<->nonzero nibble and so also perturbs it.
 * Computed ONLY when liquid is active (gated by the caller), so a still pond which
 * never ticks the fluid pass never pays for it. */
static uint64_t fluid_fill_hash(const SimState *s)
{
    const Voxel *vox = s->chunk->voxels;
    uint64_t h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        h ^= (uint64_t)(uint8_t)vox_fill(vox[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

/* Flood one connected water body of material `mat` from seed `seed` (4/6-connected
 * through SAME-material liquid cells carrying fill; held sources are walls). Marks
 * g_seen, fills g_body[0..*pn), records each distinct column (lx,lz) with its
 * floor/ceil over the body's water extent, and returns the body's TOTAL fill.
 *
 * Connectivity is through WATER ONLY (the prototype also chained open airspace
 * above the body; we deliberately omit that so the flood is BOUNDED by actual
 * water cells and cannot "vacuum the open sky" of an open-world chunk - the column
 * CEILING the body may rise into is recovered separately by flat_write_body, which
 * extends each column up through its open vessel span). */
static long flood_body(SimState *s, int seed, uint8_t mat, int *pn, int *pcols)
{
    const Voxel *vox = s->chunk->voxels;
    int n = 0, ncols = 0, qh = 0;
    long total = 0;

    g_body[n++] = seed; g_seen[seed] = 1;
    while (qh < n) {
        int i = g_body[qh++];
        int lx = i & 0x0F, ly = (i >> 4) & 0x0F, lz = (i >> 8) & 0x0F;
        int c, nn;
        total += vox_fill(vox[i]);
        for (c = 0; c < ncols; ++c)
            if (g_col_lx[c] == lx && g_col_lz[c] == lz) break;
        if (c == ncols) {
            g_col_lx[c] = lx; g_col_lz[c] = lz;
            g_col_floor[c] = ly; g_col_ceil[c] = ly; ncols++;
        }
        if (ly < g_col_floor[c]) g_col_floor[c] = ly;
        if (ly > g_col_ceil[c]) g_col_ceil[c] = ly;
        for (nn = 0; nn < 6; ++nn) {
            int nx = lx + SIM_NEIGH[nn][0];
            int ny = ly + SIM_NEIGH[nn][1];
            int nz = lz + SIM_NEIGH[nn][2];
            int nli;
            if (nx < 0 || nx >= CHUNK_DIM || ny < 0 || ny >= CHUNK_DIM ||
                nz < 0 || nz >= CHUNK_DIM)
                continue;
            nli = vox_index(nx, ny, nz);
            if (g_seen[nli]) continue;
            if (vox_mat(vox[nli]) == mat && vox_fill(vox[nli]) > 0 &&
                !is_held_source(s, nli)) {
                g_seen[nli] = 1; g_body[n++] = nli;
            }
        }
    }
    *pn = n; *pcols = ncols;
    return total;
}

/* Level the flooded body: extend every column to its full open vessel span, find
 * the flat surface level S (binary search) whose summed per-column volume equals
 * the body TOTAL, assign volumes (distributing the within-1 remainder one unit per
 * column in discovery order), and WRITE each column full-cells-bottom-up + a
 * partial top + empties above. Materialises air recipients and reverts drained
 * cells, wakes each changed cell's ring, resets its head (re-seeds next relax).
 * Redistributes the SAME total => CONSERVES EXACTLY. Returns 1 if anything moved. */
static int flat_write_body(SimState *s, int ncols, long total, uint8_t mat)
{
    Voxel *vox = s->chunk->voxels;
    int c, ly, changed = 0;
    long lo = 0, hi = 0, S, assigned = 0, rem;

    /* Extend each column through its open vessel span (stop at a solid / held cell);
     * count the column's OPEN-cell capacity in the same pass. cell_fillable skips
     * any interior obstruction, so capacity is the true fillable volume - an
     * interior shelf adds ZERO phantom capacity. */
    for (c = 0; c < ncols; ++c) {
        int lx = g_col_lx[c], lz = g_col_lz[c];
        int f = g_col_floor[c], ce = g_col_ceil[c], openc = 0;
        while (f  - 1 >= 0        && cell_fillable(s, vox_index(lx, f  - 1, lz), mat)) f--;
        while (ce + 1 < CHUNK_DIM && cell_fillable(s, vox_index(lx, ce + 1, lz), mat)) ce++;
        for (ly = f; ly <= ce; ++ly)
            if (cell_fillable(s, vox_index(lx, ly, lz), mat)) openc++;
        g_col_floor[c] = f; g_col_ceil[c] = ce;
        g_col_cap[c]   = openc * (int)FLUID_FULL;
        { long top = (long)(ce + 1) * (long)FLUID_FULL; if (top > hi) hi = top; }
    }
    /* Largest flat surface level S whose total OPEN-cell volume <= total (binary
     * search). Volume sums only fillable cells, so the level S maps correctly even
     * across interior shelves (water connected around the side equalises to S). */
    S = lo;
    while (lo < hi) {
        long mid = (lo + hi + 1) / 2, v = 0;
        for (c = 0; c < ncols; ++c) {
            int lx = g_col_lx[c], lz = g_col_lz[c];
            for (ly = g_col_floor[c]; ly <= g_col_ceil[c]; ++ly) {
                long cv;
                if (!cell_fillable(s, vox_index(lx, ly, lz), mat)) continue;
                cv = mid - (long)ly * (long)FLUID_FULL;
                if (cv < 0) cv = 0; else if (cv > (long)FLUID_FULL) cv = (long)FLUID_FULL;
                v += cv;
            }
        }
        if (v <= total) { S = mid; lo = mid; } else hi = mid - 1;
    }
    /* Per-column volume at level S (fillable cells only). */
    for (c = 0; c < ncols; ++c) {
        int lx = g_col_lx[c], lz = g_col_lz[c];
        long cvsum = 0;
        for (ly = g_col_floor[c]; ly <= g_col_ceil[c]; ++ly) {
            long cv;
            if (!cell_fillable(s, vox_index(lx, ly, lz), mat)) continue;
            cv = S - (long)ly * (long)FLUID_FULL;
            if (cv < 0) cv = 0; else if (cv > (long)FLUID_FULL) cv = (long)FLUID_FULL;
            cvsum += cv;
        }
        g_col_vol[c] = (int)cvsum; assigned += cvsum;
    }
    /* Hand the within-1 remainder out one unit per column with open headroom. */
    rem = total - assigned;
    while (rem > 0) {
        int progressed = 0;
        for (c = 0; c < ncols && rem > 0; ++c)
            if (g_col_vol[c] < g_col_cap[c]) { g_col_vol[c]++; rem--; progressed = 1; }
        if (!progressed) break;              /* cannot happen: total <= sum(open cap) */
    }
    /* Write each column: fillable cells full-bottom-up + a partial top; an interior
     * obstruction is SKIPPED WITHOUT consuming volume (so water above a shelf simply
     * resumes filling the next open cell); fillable cells above the surface revert. */
    for (c = 0; c < ncols; ++c) {
        int lx = g_col_lx[c], lz = g_col_lz[c], v = g_col_vol[c];
        for (ly = g_col_floor[c]; ly <= g_col_ceil[c]; ++ly) {
            int li = vox_index(lx, ly, lz);
            uint8_t cm = vox_mat(vox[li]);
            int put;
            /* Mark the WHOLE span seen: this body's run_finisher pass must not
             * re-flood a cell we are about to MATERIALISE (air->water, e.g. the
             * risen second tank) as a phantom new body and re-level it - that double
             * processing silently moved/lost water. */
            g_seen[li] = 1;
            if (!cell_fillable(s, li, mat)) continue;   /* solid / held / other liquid */
            put = v >= (int)FLUID_FULL ? (int)FLUID_FULL : v;
            v -= put;
            if (put > 0) {
                if (cm == MAT_AIR) {
                    fluid_occupy_air(s, li, mat, put);
                    changed = 1; wake_ring(s, li);
                } else if ((int)vox_fill(vox[li]) != put) { /* cm == mat */
                    vox_set_fill(&vox[li], (uint8_t)put);
                    changed = 1; wake_ring(s, li);
                }
            } else if (cm == mat) {          /* empty above the new surface */
                fluid_revert_to_air(s, li);
                changed = 1; wake_ring(s, li);
            }
            s->head[li] = 0;                 /* flat: head re-seeds to the level    */
        }
    }
    return changed;
}

/* Run the finisher over every connected water body in the chunk. Returns 1 if any
 * body was rewritten. Deterministic: bodies are seeded in li scan order, flooded
 * in fixed neighbour order. Held sources (lava / springs) are not free bodies and
 * are skipped as seeds. */
static int run_finisher(SimState *s)
{
    const Voxel *vox = s->chunk->voxels;
    int i, wrote = 0;
    for (i = 0; i < CHUNK_VOXELS; ++i)
        g_seen[i] = 0;                       /* (no <string.h> dependency in sim.c) */
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        uint8_t mat = vox_mat(vox[i]);
        int n = 0, ncols = 0;
        long total;
        if (g_seen[i]) continue;
        if (material_get(mat)->phase != PHASE_LIQUID || vox_fill(vox[i]) == 0)
            continue;
        if (is_held_source(s, i)) continue;
        total = flood_body(s, i, mat, &n, &ncols);
        if (flat_write_body(s, ncols, total, mat)) wrote = 1;
    }
    return wrote;
}

/* =====================================================================
 * Bind + seed  (sim.h sim_init)
 * ===================================================================== */
int sim_init(SimState *s, Chunk *c)
{
    int li;

    if (s == NULL || c == NULL)
        return 1;

    sim_build_conduct_lut();            /* idempotent; ensures the table exists */

    /* Zero everything caller-owned. SimState owns no heap in this milestone, so
     * a field-by-field clear is enough (and avoids dragging in <string.h>). */
    s->chunk      = c;
    s->act.count  = 0;
    s->act.overflow = 0;
    s->n_sources  = 0;
    s->tick_index = 0;
    s->dirty_mesh = 0;
    /* The progression event sink defaults to NULL: a freshly-init'd sim emits
     * NOTHING and is byte-identical (the read-only observer invariant). The
     * caller installs a borrowed ring AFTER sim_init via sim_set_progress_sink
     * (sim.h); sim_init never seeds it from any input. */
    s->progress   = NULL;
    for (li = 0; li < SIM_ACTIVE_MASK_WORDS; ++li)
        s->act.in_active_mask[li] = 0u;
    for (li = 0; li < SIM_ACTIVE_MASK_WORDS; ++li)
        s->moved_mask[li] = 0u;             /* fluid pass clears it per tick too */
    for (li = 0; li < SIM_MAX_SOURCES; ++li) {
        s->sources[li].active    = 0;
        s->sources[li].li        = 0;
        s->sources[li].hold_code = 0;
        s->sources[li].is_spring = 0;
    }
    /* Clear the latent accumulator side structure (ARCHITECTURE 3.8): every
     * local index starts with no transition in progress (0 == not banking). */
    for (li = 0; li < CHUNK_VOXELS; ++li)
        s->latent[li] = 0;

    /* Clear the communicating-vessels head field (transient acceleration state,
     * NOT persisted): a fresh chunk starts with head 0 everywhere (no surface
     * pressure recorded yet) and head_relax rebuilds it over the active front. A
     * 0 head means sim_liquid_unsettled's rise test never fires at seed time, so a
     * flat seeded pond still sleeps for free. */
    for (li = 0; li < CHUNK_VOXELS; ++li) {
        s->head[li] = 0;
    }
    /* Clear the finisher limit-cycle memory (transient): no cycle observed yet,
     * nothing snapped yet. */
    for (li = 0; li < FLUID_RING_N; ++li)
        s->fluid_ring[li] = 0;
    for (li = 0; li < FLUID_FIRED_MAX; ++li)
        s->fluid_fired[li] = 0;
    s->fluid_ring_fill = 0;
    s->fluid_ring_pos  = 0;
    s->fluid_cyc_seen  = 0;
    s->fluid_n_fired   = 0;
    s->fluid_down       = 3;   /* 0.5 M3: -Y default (flat gravity for tests/proto) */
    s->fluid_finisher_on = 0;  /* 0.5 M3: terraced binary flow; M4 enables the snap */
    s->fluid_xfn        = NULL; /* 0.5 M4: no cross-chunk flow until the WorldCA sets it */
    s->fluid_xfn_user   = NULL;

    /* Seed the authoritative full-resolution temperature heat[] from every
     * voxel's stored 8-bit code (ARCHITECTURE 3.4 mitigation). heat[] is kept
     * valid for ALL voxels at all times - this whole-array seed plus committing
     * only through heat[] means a freshly-woken voxel already has a correct
     * value, so wake needs no special init. Held sources are re-synced from
     * their hold code by sim_set_source just below. */
    for (li = 0; li < CHUNK_VOXELS; ++li)
        s->heat[li] = temp_to_heat(vox_temp_code(c->voxels[li]));

    /* Clear any stale VF_ACTIVE flags left on the chunk before we re-seed (the
     * list+mask must agree with the flag, and we are about to rebuild both). */
    for (li = 0; li < CHUNK_VOXELS; ++li) {
        Voxel *v = &c->voxels[li];
        if (vox_flags(*v) & VF_ACTIVE)
            vox_set_flags(v, vox_flags(*v) & ~VF_ACTIVE);
    }

    /* Hold every MAT_EMISSIVE voxel (lava) at the binding hold temperature: it
     * is a Dirichlet boundary that drives the sim (sim.h section 5). Stamp BOTH
     * the 8-bit code and the authoritative heat[] to the hold, and wake it so it
     * diffuses into its neighbours from tick 0. Holding is governed by the
     * MAT_EMISSIVE flag (see is_held_source / held_code_of), so a lava POOL of ANY
     * size is held in full - the whole pool stays hot, not just the first
     * SIM_MAX_SOURCES voxels (without this a copper slab resting on a large pool
     * sits on cold, unheld lava and never warms).
     *
     * We deliberately do NOT register emissive lava into the explicit source table:
     * the flag already drives holding, so slotting lava was pure redundancy that
     * consumed all SIM_MAX_SOURCES slots on a forge pool and STARVED real held
     * sources/springs (a demo water spring placed in the forge chunk could not
     * register -> sim_set_spring returned "table full" -> it never flowed). The
     * table is now reserved for explicit sim_set_source / sim_set_spring registrations
     * (springs, player-placed sources); emissive lava costs zero slots. */
    {
        uint8_t lava_hold = g_lava_hold_code;   /* cached in the LUT build above */
        for (li = 0; li < CHUNK_VOXELS; ++li) {
            Voxel *v = &c->voxels[li];
            if (!(material_get(vox_mat(*v))->flags & MAT_EMISSIVE))
                continue;
            vox_set_temp_code(v, lava_hold);
            s->heat[li] = temp_to_heat(lava_hold);
            active_enqueue(s, li);              /* held by flag; needs no table slot */
        }
    }

    /* Auto-register every MAT_SPRING voxel (a placed/loaded water source) as an
     * inexhaustible spring: the MATERIAL is the spring (persisted in mat8), so a
     * source survives save/reload and floods without an external sim_set_spring call.
     * Held at its own (ambient) temperature, so it never boils. Unlike emissive lava
     * a spring DOES take a source-table slot (it carries per-cell spring state); the
     * table comfortably holds SIM_MAX_SOURCES springs per chunk now that lava is
     * flag-held. */
    for (li = 0; li < CHUNK_VOXELS; ++li) {
        Voxel *v = &c->voxels[li];
        if (material_get(vox_mat(*v))->flags & MAT_SPRING)
            sim_set_spring(s, (uint16_t)li, vox_temp_code(*v));
    }

    /* Seed the active front: wake every voxel that differs from an in-chunk
     * neighbour by more than SIM_WAKE_QUANTUM (so a freshly-placed hot source,
     * and the cold neighbours it can push into, are awake from tick 0). The
     * comparison is in heat units, exactly the wake test the tick uses. */
    {
        int lx, ly, lz, n;
        for (lz = 0; lz < CHUNK_DIM; ++lz) {
            for (ly = 0; ly < CHUNK_DIM; ++ly) {
                for (lx = 0; lx < CHUNK_DIM; ++lx) {
                    int idx = vox_index(lx, ly, lz);
                    int32_t Ti = temp_to_heat(vox_temp_code(c->voxels[idx]));
                    for (n = 0; n < 6; ++n) {
                        int nx = lx + SIM_NEIGH[n][0];
                        int ny = ly + SIM_NEIGH[n][1];
                        int nz = lz + SIM_NEIGH[n][2];
                        int32_t Tj, d;
                        if (nx < 0 || nx >= CHUNK_DIM ||
                            ny < 0 || ny >= CHUNK_DIM ||
                            nz < 0 || nz >= CHUNK_DIM)
                            continue;                  /* closed boundary */
                        Tj = temp_to_heat(vox_temp_code(
                                 c->voxels[vox_index(nx, ny, nz)]));
                        d = Tj - Ti;
                        if (d < 0) d = -d;
                        if (d > SIM_WAKE_QUANTUM) {
                            active_enqueue(s, idx);
                            break;             /* one wake per voxel is enough */
                        }
                    }
                }
            }
        }
    }

    /* Also wake every UNSETTLED liquid (fluid milestone): a liquid placed in a
     * uniform-temperature chunk has no heat gradient to wake it, but it must be
     * on the front so PHASE 1.6 can flow it from tick 0. A settled level pool
     * (sim_liquid_unsettled == 0) is NOT woken, preserving the "still pond costs
     * nothing" property: a chunk seeded with only a flat, settled liquid surface
     * still yields zero active voxels. Reads only fill/phase, never heat. */
    for (li = 0; li < CHUNK_VOXELS; ++li) {
        if (sim_liquid_unsettled(s, (uint16_t)li))
            active_enqueue(s, li);
    }

    s->chunk->active_count = s->act.count;
    return 0;
}

/* =====================================================================
 * One heat tick  (sim.h sim_tick - read / commit / compact)
 * ===================================================================== */
void sim_tick_ex(SimState *s, int phases, SimNeighFn nfn, SimWakeFn wfn, void *user)
{
    ChunkActive *act;
    Voxel *vox;
    uint32_t n_writes = 0;
    uint32_t i;
    int changed = 0;

    if (s == NULL || s->chunk == NULL)
        return;

    act = &s->act;
    vox = s->chunk->voxels;

    /* Snapshot the active count at start-of-tick. Waking a neighbour appends to
     * act->active[] beyond this bound; capturing read_count means the read pass
     * processes ONLY voxels active at the start of this tick, so a just-woken
     * neighbour is first read NEXT tick - the front advances exactly one ring
     * per tick and the read pass remains a true simultaneous FTCS step. */
    if (phases & SIM_PHASE_READ) {
        uint32_t read_count = act->count;

    /* ---- PHASE 1: read (double-buffered) -----------------------------------
     * For each active voxel (up to SIM_ACTIVE_CAP) compute its new temperature
     * from START-OF-TICK neighbour temperatures, accumulating face flux in an
     * int64 (a single term can reach ~2.9e9 > int32, the six-term sum ~1.7e10 -
     * sim.h overflow note, load-bearing). Held sources are not diffused here;
     * they are re-stamped in phase 2. We also wake neighbours into which this
     * voxel pushes more than SIM_WAKE_QUANTUM of flux, advancing the front one
     * ring per tick. Reads-only this pass: no commit until phase 2, so the step
     * is order-independent (true simultaneous FTCS update). */
    for (i = 0; i < read_count && n_writes < SIM_ACTIVE_CAP; ++i) {
        int li = act->active[i];
        int lx = li & 0x0F;
        int ly = (li >> 4) & 0x0F;
        int lz = (li >> 8) & 0x0F;
        uint8_t mi = vox_mat(vox[li]);
        /* Read the FULL-PRECISION temperature from the authoritative heat[], not
         * by re-decoding the 8-bit code: this is what lets a sub-quantum per-tick
         * rise accumulate across ticks (ARCHITECTURE 3.4 mitigation). */
        int32_t Ti = s->heat[li];
        int64_t flux = 0;
        int32_t dT, Tnew;
        int delivers = 0;    /* does this voxel push >=1 heat-unit into a neigh? */
        int n;
        int wake_nli[6];     /* neighbours to wake (recorded as we scan faces)    */
        int n_wake = 0;

        for (n = 0; n < 6; ++n) {
            int nx = lx + SIM_NEIGH[n][0];
            int ny = ly + SIM_NEIGH[n][1];
            int nz = lz + SIM_NEIGH[n][2];
            int nli;
            uint8_t nmi;
            int32_t Tj, diff;
            int64_t into;

            if (nx < 0 || nx >= CHUNK_DIM ||
                ny < 0 || ny >= CHUNK_DIM ||
                nz < 0 || nz >= CHUNK_DIM) {
                /* 0.4 M4: a CROSS-CHUNK face. nfn (NULL => closed wall, the 0.3
                 * single-chunk path) returns the neighbour voxel's START-OF-TICK
                 * heat + material from the adjacent chunk; since all active chunks
                 * READ before any COMMIT, this is the neighbour's start-of-tick
                 * value, so the seam diffuses exactly like an interior face (same
                 * kw, order-independent). The neighbour's LOCAL coords wrap the
                 * single out-of-bounds axis. */
                int nlx = (nx + CHUNK_DIM) & (CHUNK_DIM - 1);
                int nly = (ny + CHUNK_DIM) & (CHUNK_DIM - 1);
                int nlz = (nz + CHUNK_DIM) & (CHUNK_DIM - 1);
                /* SIM_NEIGH order is +X,-X,+Y,-Y,+Z,-Z but the Chunk neigh[] /
                 * Face-enum order is -X,+X,-Y,+Y,-Z,+Z - swapped within each axis
                 * pair, so the neighbour-chunk face index is exactly (n ^ 1). */
                int face = n ^ 1;
                int32_t Tn;
                uint8_t nmt;
                int64_t into_x;
                if (nfn == NULL || !nfn(user, s, face, nlx, nly, nlz, &Tn, &nmt))
                    continue;           /* closed wall: no neighbour to exchange  */
                flux += (int64_t)g_conduct_lut[mi][nmt] * (int64_t)(Tn - Ti);
                /* Deliver-flux test across the seam: if we push the neighbour a
                 * quantum, wake the neighbour CHUNK (enqueue-only; the WorldCA
                 * acts after commit, so it first ticks next world-tick). */
                into_x = (int64_t)g_conduct_lut[nmt][mi] * (int64_t)(Ti - Tn);
                {
                    int64_t into_dT = into_x >> HEAT_SHIFT;
                    if (into_dT < 0) into_dT = -into_dT;
                    if (into_dT > SIM_WAKE_QUANTUM) {
                        delivers = 1;
                        if (wfn != NULL)
                            wfn(user, s, face, nlx, nly, nlz);
                    }
                }
                continue;
            }

            nli  = vox_index(nx, ny, nz);
            nmi  = vox_mat(vox[nli]);
            /* Neighbour temperature ALSO read at full precision from heat[] (the
             * start-of-tick value: the read pass never writes heat[], preserving
             * the true simultaneous FTCS double-buffer). */
            Tj   = s->heat[nli];
            diff = Tj - Ti;

            /* Flux INTO this voxel across the face. kw is asymmetric: kw[mi][nmi]
             * divides the face flux by THIS voxel's own volumetric heat capacity
             * (this voxel is the receiver). */
            flux += (int64_t)g_conduct_lut[mi][nmi] * (int64_t)diff;

            /* Flux this voxel DELIVERS into the neighbour across the same face,
             * with the NEIGHBOUR as the receiver: kw[nmi][mi]*(Ti-Tj). Wake the
             * neighbour only when that delivered flux is more than a quantum's
             * worth (sim.h: "wakes the neighbours it pushed more than
             * SIM_WAKE_QUANTUM-equivalent flux into"). The currency is DELIVERED
             * flux in heat units, and the threshold is SIM_WAKE_QUANTUM (one
             * code's worth, = HEAT_ONE_C) - NOT a raw temperature difference and
             * NOT merely-nonzero: across a poor conductor a static multi-degree
             * gap delivers sub-quantum flux that the receiver re-quantizes away
             * every tick, so it must NOT re-wake. Otherwise a stalled gradient
             * never quiesces and idle voxels never cost zero (the load-bearing
             * equilibrated-chunk-is-free property). */
            into = (int64_t)g_conduct_lut[nmi][mi] * (int64_t)(Ti - Tj);
            {
                /* Right shift of a (routinely negative) signed int64 here and at
                 * the `flux >> HEAT_SHIFT` below: we RELY on an ARITHMETIC shift
                 * (round toward -inf, sign-extending). C99 leaves >> on negative
                 * signed values implementation-defined, but both committed ship
                 * targets - Linux gcc and i686-w64-mingw32 gcc (Decision Ledger
                 * names x86/GCC on both sides) - emit a sign-extending arithmetic
                 * shift, so this is correct on every platform we build for. We do
                 * NOT branch on the sign as heat_to_code_i does, because matching
                 * floor-toward-negative-infinity portably is fiddly and any naive
                 * toward-zero replacement would change FTCS output and break the
                 * sim tests; on the fixed targets the bare shift is exact. */
                int64_t into_dT = into >> HEAT_SHIFT;
                if (into_dT < 0) into_dT = -into_dT;
                if (into_dT > SIM_WAKE_QUANTUM) {
                    delivers = 1;
                    wake_nli[n_wake++] = nli;
                }
            }
        }

        dT   = (int32_t)(flux >> HEAT_SHIFT);   /* back to heat units */
        Tnew = Ti + dT;

        /* Clamp to the codec's representable range in heat units (-40..2020 C)
         * so a write can never encode out of range. The stability proof keeps
         * the physical solution inside this band already; the clamp is belt-
         * and-braces against quantization at the extremes. */
        if (Tnew < HEAT_RANGE_MIN) Tnew = HEAT_RANGE_MIN;
        if (Tnew > HEAT_RANGE_MAX) Tnew = HEAT_RANGE_MAX;

        /* Queue the write (double-buffer): never commit (neither heat[] nor the
         * code) during the read pass - that preserves the simultaneous FTCS step.
         * We carry the PRECISE Tnew (heat units) as the authoritative value and
         * the derived 8-bit code for storage/mesh. Held sources are queued too
         * but ignored at commit (re-stamped). The integer encoder keeps the
         * commit float-free (Decision Ledger). */
        s->writes[n_writes].li         = (uint16_t)li;
        s->writes[n_writes].t_new_heat = Tnew;
        s->writes[n_writes].t_new      = heat_to_code_i(Tnew);
        ++n_writes;

        /* Apply the deferred neighbour wakes now (after this voxel's write is
         * queued). active_enqueue appends beyond read_count, so the wakes take
         * effect NEXT tick - the front advances exactly one ring per tick and
         * the read pass stays a true simultaneous step over the start-of-tick
         * active set. */
        {
            int w;
            for (w = 0; w < n_wake; ++w)
                active_enqueue(s, wake_nli[w]);
        }

        /* LAZY SLEEP (sim.h section 3): a voxel sleeps ONLY when it is truly
         * equilibrated - its PRECISE incoming flux is EXACTLY zero (no net heat
         * crossing any face, evaluated against the full-resolution heat[] values,
         * NOT the re-quantized code) AND it delivers no >=1-heat-unit flux into a
         * neighbour. The precise-flux test is the ARCHITECTURE 3.4 fix: a real
         * sub-quantum gradient (flux != 0 but flux >> HEAT_SHIFT rounds to 0)
         * keeps the voxel AWAKE so its heat[] inches up across ticks rather than
         * stalling - the old code slept on dT == 0 (the rounded value), which is
         * exactly how sub-quantum heating from a poor conductor was lost. Only a
         * voxel whose neighbours all sit at its own precise temperature (flux
         * == 0) is genuinely done, so a uniform/equilibrated chunk still costs
         * zero. Clear VF_ACTIVE now; the index is physically dropped in phase 3
         * compaction so active[] order stays stable through the sweep. A held
         * source is never slept (always awake). A voxel mid-transition
         * (s->latent[li] != 0) is never slept either: it sits at the melt/freeze
         * plateau and must keep ticking so the PHASE 1.5 pass can finish filling
         * the latent bank (otherwise the plateau, whose dT rounds to zero, would
         * sleep forever and never flip phase). */
        if (flux == 0 && !delivers && !is_held_source(s, li) &&
            s->latent[li] == 0 && !sim_liquid_unsettled(s, li)) {
            vox_set_flags(&vox[li], vox_flags(vox[li]) & ~VF_ACTIVE);
            mask_clear(act, li);
        }
    }
    }   /* end PHASE 1 (read pass) */

    /* 0.4 M4: hand the pending write count READ -> COMMIT, and gate the commit.
     * A read-only call stops here (the WorldCA runs every chunk's READ before any
     * COMMIT, so cross-chunk reads above saw start-of-tick state). A commit-only
     * call loads the count the earlier read pass stored. The single-call wrapper
     * (sim_tick) runs both, so n_writes is already live. */
    if (phases & SIM_PHASE_READ)
        s->n_writes = n_writes;
    if (!(phases & SIM_PHASE_COMMIT))
        return;
    if (!(phases & SIM_PHASE_READ))
        n_writes = s->n_writes;

    /* ---- PHASE 2: commit + re-stamp held sources ---------------------------
     * Commit every queued write THROUGH the authoritative heat[]: store the
     * PRECISE t_new_heat first (so a sub-quantum rise persists across ticks -
     * the ARCHITECTURE 3.4 mitigation), then derive the 8-bit storage/mesh code
     * from it and update the voxel code only when it actually moved a quantum.
     * A held Dirichlet boundary (explicit source OR MAT_EMISSIVE lava) is NOT
     * committed from its diffusion write - it is re-stamped to its hold code
     * (never cools, keeps driving diffusion) and kept awake instead. A change to
     * any committed code flips dirty_mesh so main.c remeshes (glow tracks temp). */
    for (i = 0; i < n_writes; ++i) {
        int li = s->writes[i].li;
        uint8_t old_code, new_code;

        if (is_held_source(s, li)) {
            /* Re-stamp the boundary to its hold (code + precise heat) and keep
             * it awake so it remains on the front driving diffusion next tick. */
            uint8_t hold = held_code_of(s, li);
            s->heat[li] = temp_to_heat(hold);
            if (vox_temp_code(vox[li]) != hold) {
                vox_set_temp_code(&vox[li], hold);
                changed = 1;
            }
            /* Only a SPRING is re-filled to brim (fill=15) each tick - the mass
             * analogue of the temperature hold, flooding outward without draining.
             * A plain held source (incl. MAT_EMISSIVE lava) holds HEAT ONLY: its
             * fill is conserved, so it settles into a pool instead of flooding
             * (the held-heat / held-fill decoupling). */
            if (is_spring_source(s, li) && vox_fill(vox[li]) != FLUID_FULL) {
                vox_set_fill(&vox[li], FLUID_FULL);
                changed = 1;
            }
            active_enqueue(s, li);
            continue;
        }

        /* The precise value is the truth; persist it unconditionally. */
        s->heat[li] = s->writes[i].t_new_heat;

        /* Derive the storage code from the precise heat and update the voxel
         * code only when it crossed a code boundary (so the glow + persistence
         * track temperature; sub-quantum ticks keep accumulating silently). */
        old_code = vox_temp_code(vox[li]);
        new_code = s->writes[i].t_new;     /* == heat_to_code_i(t_new_heat) */
        if (new_code != old_code) {
            vox_set_temp_code(&vox[li], new_code);
            changed = 1;
            /* ---- Progression OBSERVER emit (READ-ONLY side output) ----------
             * The committed temp CODE moved. If it first CROSSED UPWARD a coarse
             * tier milestone (200/500/1000/1500 C, precomputed to codes once in
             * g_tier_code[]) that old_code had not yet reached, emit PROG_TEMP_TIER
             * carrying the band code crossed - the capability gate, observed as it
             * happens. A single rise can cross several bands, so emit each. FLOAT-
             * FREE: the hot loop compares 8-bit codes, never Celsius (the encode
             * ran once at LUT-build). GATED on the optional sink: when NULL the
             * events are not built, so the sim is byte-identical (Section 9). */
            if (s->progress) {
                int t;
                for (t = 0; t < 4; ++t) {
                    uint8_t band = g_tier_code[t];
                    if (old_code < band && new_code >= band) {
                        int lx = li & 0x0F, ly = (li >> 4) & 0x0F, lz = (li >> 8) & 0x0F;
                        ProgressEvent ev;
                        ev.kind               = (uint8_t)PROG_TEMP_TIER;
                        ev.material           = vox_mat(vox[li]);
                        ev.observed_temp_code = new_code;
                        ev.tier_code          = band;
                        ev.wx = s->chunk->cx * CHUNK_DIM + lx;
                        ev.wy = s->chunk->cy * CHUNK_DIM + ly;
                        ev.wz = s->chunk->cz * CHUNK_DIM + lz;
                        ev.tick = (uint32_t)s->tick_index;
                        prog_emit(s->progress, &ev);
                    }
                }
            }
        }
    }

    /* Re-stamp every EXPLICIT registered source after the commit so it always
     * reads its hold even if no diffusion write was queued for it this tick
     * (e.g. it was list-dropped on overflow, or was never read). Sync heat[] to
     * the precise hold value too (the Dirichlet boundary truth) and keep it
     * awake. (MAT_EMISSIVE boundaries are re-stamped in the write loop above
     * when active; a never-active emissive voxel sits in a uniform region and
     * needs no stamping until a neighbour wakes it.) */
    for (i = 0; i < (uint32_t)s->n_sources; ++i) {
        if (!s->sources[i].active)
            continue;
        {
            int li = s->sources[i].li;
            uint8_t hold = s->sources[i].hold_code;
            s->heat[li] = temp_to_heat(hold);
            if (vox_temp_code(vox[li]) != hold) {
                vox_set_temp_code(&vox[li], hold);
                changed = 1;
            }
            /* Re-fill only if this source is a SPRING (mirrors the write-loop
             * branch) - so a never-read spring still floods, while a plain held
             * source / lava holds heat only and keeps its conserved fill. */
            if (s->sources[i].is_spring && vox_fill(vox[li]) != FLUID_FULL) {
                vox_set_fill(&vox[li], FLUID_FULL);
                changed = 1;
            }
            active_enqueue(s, li);
        }
    }

    /* ---- PHASE 1.5: state transitions (melt / solidify, latent banking) ----
     * ARCHITECTURE 3.7 ordering: heat first (PHASE 2 commit, above), THEN
     * transitions against the freshly-committed temperatures, BEFORE compaction
     * (PHASE 3). Iterate the active front; a voxel mid-transition stays awake
     * (the PHASE 1 sleep guard keeps any latent[li] != 0 voxel on the list), so
     * the whole banking set is reachable through act->active[]. Held sources are
     * a Dirichlet boundary stamped to their hold each tick and must never melt
     * or freeze themselves, so they are skipped. try_phase_change reads only
     * MaterialDef thresholds (no id switch); a swap of material id and/or
     * VF_LIQUID flips changed so dirty_mesh fires and main.c remeshes (the new
     * molten color + temperature glow then appear via the existing dirty path).
     *
     * Snapshot the count first: try_phase_change -> wake_ring appends to
     * act->active[] (the now-molten voxel + its ring), and those re-evaluate
     * NEXT tick - this pass touches only voxels active at start-of-tick, exactly
     * like the read pass, so it terminates and the front advances one ring. */
    {
        uint32_t trans_count = act->count;
        for (i = 0; i < trans_count; ++i) {
            int li = act->active[i];
            if (is_held_source(s, li))
                continue;               /* held Dirichlet boundary: never melts */
            if (try_phase_change(s, li))
                changed = 1;
        }
    }

    /* ---- PHASE 1.6: fluid flow (integer gravity + lateral equalise) --------
     * Runs AFTER PHASE 1.5 transitions (so a voxel that just melted this tick -
     * now PHASE_LIQUID with fill=15 - flows in the SAME machinery) and BEFORE
     * PHASE 3 compaction. In-place, conservative, deterministic (fluid design).
     *
     * Clear the per-tick moved bitmask, then snapshot fluid_count = act->count
     * (like the read and transition passes): cells woken during the pass (the
     * recipients of fill, via wake_ring) re-evaluate NEXT tick, so the fluid
     * front advances one ring/tick and the pass terminates over a fixed set.
     *
     * Per active liquid voxel in act->active[] order: skip non-PHASE_LIQUID
     * voxels (read MaterialDef.phase - never an id switch); skip a voxel already
     * marked moved this tick (it received fill and must not double-move). A held
     * liquid source is NOT skipped: it DONATES outward (is_source=1 so it is
     * never drained to air and is re-filled to 15 next tick in PHASE 2). Any fill
     * or material-id change flips `changed` so dirty_mesh fires and main.c
     * remeshes via the existing dirty path. Single-chunk scope: out-of-chunk
     * neighbours are a closed wall (CROSS-CHUNK FLUID FLOW IS DEFERRED, exactly
     * like cross-chunk heat). The pass shares SIM_ACTIVE_CAP graceful-slowdown:
     * leftover unsettled liquid simply stays active for next tick. */
    {
        uint32_t fluid_count = act->count;
        int wi;
        int fluid_changed = 0;
        int has_free_liquid = 0;
        for (wi = 0; wi < SIM_ACTIVE_MASK_WORDS; ++wi)
            s->moved_mask[wi] = 0u;

        /* Is any ACTIVE liquid actually free to flow (PHASE_LIQUID and NOT a held
         * source)? The communicating-vessels machinery - the head flood and the
         * stuck-body finisher trigger - is meaningful only then. A chunk whose only
         * active liquid is HELD lava (re-enqueued every tick, so the chunk never
         * sleeps) would otherwise pay a whole-chunk head flood + a 4096-voxel fill
         * hash every tick forever, for nothing. */
        {
            uint32_t fi;
            for (fi = 0; fi < fluid_count; ++fi) {
                int li = act->active[fi];
                if (material_get(vox_mat(vox[li]))->phase == PHASE_LIQUID &&
                    !is_held_source(s, li)) { has_free_liquid = 1; break; }
            }
        }

        /* HEAD field (per-body max free surface) BEFORE the flow + sleep passes, so
         * sim_liquid_unsettled's rise clause reads a head consistent with this
         * tick's surfaces and keeps an un-levelled connected body AWAKE (a second
         * tank still below a taller first tank joined by a bottom channel) until the
         * finisher levels it. The local gravity+lateral rule cannot raise a column
         * against gravity; the connected-body finisher (below) does, as a snap. */
        /* 0.5 M3: the head field + connected-body finisher are PARTIAL-FILL,
         * world-Y machinery. The binary flow rule settles on its own (terraced),
         * so they are OFF here (fluid_finisher_on==0). M4 replaces run_finisher
         * with a RADIAL shell-snap and turns this on. Kept compiled + referenced
         * (so no -Wunused) but not exercised in M3. */
        if (s->fluid_finisher_on && has_free_liquid)
            head_relax(s);

        for (i = 0; i < fluid_count; ++i) {
            int li = act->active[i];
            if (material_get(vox_mat(vox[li]))->phase != PHASE_LIQUID)
                continue;               /* data-driven: only liquids flow */
            if (moved_test(s, li))
                continue;               /* already received fill this tick */
            /* Pass the held flag AND the spring flag distinctly: fluid_step
             * keeps a held source from reverting to air at fill=0, but only a
             * SPRING is allowed to flow/donate (a non-spring held source - incl.
             * MAT_EMISSIVE lava - conserves fill in place; see fluid_step). */
            if (fluid_step(s, li, is_held_source(s, li), is_spring_source(s, li)))
                fluid_changed = 1;
        }

        /* CONNECTED-BODY FINISHER trigger (Approach B). The local gravity+lateral
         * rule leaves an un-levelled connected body STUCK: either statically (a
         * second tank's intake row filled to within-1 of the first, so lateral
         * stops, while the head rise-clause keeps it awake) or in a sub-cell LIMIT
         * CYCLE (the equilibrium surface falls between integer cell levels). Both
         * show up as a fill-state hash that RECURS in a small ring. When the body is
         * stuck (hash recurs) for FLUID_CYCLE_CONFIRM consecutive ticks, fire ONE
         * bounded connected-body flat-snap that levels every body EXACTLY (conserving
         * its total). We do NOT gate on fluid_changed: a static stall (the common
         * communicating-vessels case) has fluid_changed==0 yet must still fire - the
         * stable head field keeps the body awake long enough to confirm the stall.
         * Guard on the post-snap hash so we never re-fire an equilibrium already
         * levelled (O(1) fires per disturbance). Gated on has_free_liquid: a still
         * pond or a lava-only chunk never hashes, so it costs nothing there.
         * 0.5 M3: OFF (fluid_finisher_on==0) - binary flow needs no finisher to
         * settle (terraced). M4 radializes run_finisher + enables this. */
        if (s->fluid_finisher_on && has_free_liquid) {
            uint64_t hh = fluid_fill_hash(s);
            int in_cycle = 0, j;
            for (j = 0; j < (int)s->fluid_ring_fill; ++j)
                if (s->fluid_ring[j] == hh) { in_cycle = 1; break; }
            s->fluid_ring[s->fluid_ring_pos] = hh;
            s->fluid_ring_pos = (uint16_t)((s->fluid_ring_pos + 1) % FLUID_RING_N);
            if (s->fluid_ring_fill < FLUID_RING_N) s->fluid_ring_fill++;
            if (in_cycle) s->fluid_cyc_seen++;
            else          s->fluid_cyc_seen = 0;
            if (s->fluid_cyc_seen >= FLUID_CYCLE_CONFIRM) {
                int already = 0;
                for (j = 0; j < (int)s->fluid_n_fired; ++j)
                    if (s->fluid_fired[j] == hh) { already = 1; break; }
                if (!already) {
                    int wrote = run_finisher(s);
                    uint64_t fh = fluid_fill_hash(s);   /* post-snap signature */
                    if (s->fluid_n_fired < FLUID_FIRED_MAX)
                        s->fluid_fired[s->fluid_n_fired++] = fh;
                    if (wrote) fluid_changed = 1;
                    s->fluid_ring_fill = 0;
                    s->fluid_ring_pos  = 0;
                    s->fluid_cyc_seen  = 0;
                }
            }
        }

        if (fluid_changed)
            changed = 1;
    }

    /* ---- PHASE 3: compaction (drop lazily-slept indices, keep order) -------
     * Walk active[] in order, keeping each VF_ACTIVE voxel exactly once, in its
     * original relative order (so the double-buffer read order is stable next
     * tick). Slept voxels cleared their flag + mask in phase 1 and are dropped.
     *
     * DEDUPE: a voxel that slept earlier in this tick and was then re-woken by a
     * later voxel (active_enqueue re-set its flag + mask AND appended a second
     * copy) now appears twice. We use the mask bit as a "already retained this
     * tick" marker: on the first sighting of a live voxel we retain it and CLEAR
     * its mask; any later duplicate then sees mask==0 and is skipped without
     * touching the flag (the retained copy still owns it). After the sweep we
     * re-stamp the mask for exactly the survivors so list and mask agree again. */
    {
        uint16_t w = 0;
        uint16_t k;
        for (i = 0; i < act->count; ++i) {
            int li = act->active[i];
            if ((vox_flags(vox[li]) & VF_ACTIVE) && mask_test(act, li)) {
                act->active[w++] = (uint16_t)li;
                mask_clear(act, li);        /* mark retained: dedupe duplicates */
            } else if (!(vox_flags(vox[li]) & VF_ACTIVE)) {
                mask_clear(act, li);        /* genuine sleep: drop flag's mate   */
            }
            /* else: live flag but mask already cleared == duplicate; skip. */
        }
        act->count = w;
        for (k = 0; k < w; ++k)             /* restore mask for the survivors    */
            mask_set(act, act->active[k]);
    }

    s->chunk->active_count = act->count;
    if (changed) {
        s->dirty_mesh = 1;
        s->chunk->flags |= CHUNK_DIRTY_MESH;
    }
}

/* The single-chunk tick: READ + COMMIT in one call with closed walls (no
 * cross-chunk neighbour fn), then advance this sim's own tick counter. Every
 * pre-0.4 caller (tests, the warm-up soak) uses this and is byte-identical to
 * 0.3. The WorldCA instead drives sim_tick_ex's two phases across all active
 * chunks and feeds tick_index from the shared WorldClock. */
void sim_tick(SimState *s)
{
    if (s == NULL || s->chunk == NULL)
        return;
    sim_tick_ex(s, SIM_PHASE_READ | SIM_PHASE_COMMIT, NULL, NULL, NULL);
    ++s->tick_index;
}

/* =====================================================================
 * Teardown  (sim.h sim_shutdown)
 * ===================================================================== */
/* A player edit changed voxel li in the simulated chunk out from under the CA
 * (a block broken to air, or a new block placed). Re-seed the authoritative
 * heat[] from the voxel's now-current temperature code, clear any in-progress
 * latent bank (the material/phase may have changed), and wake the cell plus its
 * 6 face neighbours so heat diffusion and fluid flow re-evaluate around the edit
 * on the next tick. main.c calls this only when the edited chunk is the one this
 * SimState is bound to (s->chunk), so the world layer needs no sim dependency. */
void sim_notify_edit(SimState *s, int li)
{
    if (s == NULL || s->chunk == NULL)
        return;
    if (li < 0 || li >= CHUNK_VOXELS)
        return;

    /* If this cell was a registered Dirichlet SOURCE (sim_init auto-registers the
     * first SIM_MAX_SOURCES emissive lava cells) and the edit replaced it with a
     * NON-emissive voxel (a placed block, or air from a break), retire the slot.
     * Otherwise the PHASE-2 re-stamp loop would keep pinning this cell to the
     * lava hold temperature every tick - an invisible heat "ghost" that never
     * quiesces. A cell that is STILL emissive (lava placed back) stays held via
     * the MAT_EMISSIVE flag path (is_held_source) and needs no slot. */
    {
        int idx = source_index_of(s, li);
        if (idx >= 0 &&
            !(material_get(vox_mat(s->chunk->voxels[li]))->flags & MAT_EMISSIVE)) {
            s->sources[idx].active    = 0;
            s->sources[idx].li        = 0;
            s->sources[idx].hold_code = 0;
            s->sources[idx].is_spring = 0;
        }
    }

    s->heat[li]   = temp_to_heat(vox_temp_code(s->chunk->voxels[li]));
    s->latent[li] = 0;
    s->head[li]   = 0;       /* surfaces moved: re-seed head from scratch */
    /* A player edit is a fresh disturbance: forget the limit-cycle history so a new
     * equilibrium is re-detected and the finisher can fire again for it (the
     * already-fired set persists - a genuinely identical equilibrium is still a
     * no-op, but a NEW one after the edit is no longer masked by a stale ring). */
    s->fluid_ring_fill = 0;
    s->fluid_ring_pos  = 0;
    s->fluid_cyc_seen  = 0;
    active_enqueue(s, li);   /* wake the edited cell itself */
    wake_ring(s, li);        /* and its 6 face neighbours */
}

void sim_shutdown(SimState *s)
{
    int i;
    if (s == NULL)
        return;

    /* Clear the VF_ACTIVE flags we set on the chunk so a re-init (or a handoff
     * to another subsystem) sees clean ground-truth flags, then drop the front
     * and source tables. SimState owns no heap in this milestone, so this is
     * symmetry with sim_init (forward-compat) rather than a free(). */
    /* 0.5 M1: tolerate a chunk whose voxel block is GONE. A SimState can outlive
     * its chunk's eviction (e.g. world_shutdown evicts everything before the
     * WorldCA tears down g_xsim), and an evicted/uniform chunk has voxels == NULL.
     * There is no live block to clear VF_ACTIVE on (and the chunk is being
     * destroyed anyway), so skip - pre-0.5 this read stale-but-valid inline memory. */
    if (s->chunk != NULL && s->chunk->voxels != NULL) {
        for (i = 0; i < (int)s->act.count; ++i) {
            int li = s->act.active[i];
            Voxel *v = &s->chunk->voxels[li];
            vox_set_flags(v, vox_flags(*v) & ~VF_ACTIVE);
        }
        s->chunk->active_count = 0;
    }

    s->act.count    = 0;
    s->act.overflow = 0;
    for (i = 0; i < SIM_ACTIVE_MASK_WORDS; ++i)
        s->act.in_active_mask[i] = 0u;
    for (i = 0; i < SIM_ACTIVE_MASK_WORDS; ++i)
        s->moved_mask[i] = 0u;              /* hygiene: per-tick scratch */
    for (i = 0; i < SIM_MAX_SOURCES; ++i)
        s->sources[i].active = 0;
    /* Drop any in-progress latent banking and the full-resolution temperature
     * mirror for symmetry with sim_init (heat[] is re-seeded from voxel codes on
     * the next sim_init, so zeroing here is just hygiene). */
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        s->latent[i] = 0;
        s->heat[i]   = 0;
    }
    s->n_sources  = 0;
    s->dirty_mesh = 0;
    s->chunk      = NULL;
}

/* ---- Test-only determinism hash (0.4 M0; VOXEL_DETERMINISM_HARNESS) -------- *
 * See sim.h. FNV-1a 64-bit over the bound chunk's voxels + the authoritative
 * heat[]/latent[] side arrays. Compiled out of release builds entirely. */
#ifdef VOXEL_DETERMINISM_HARNESS
static uint64_t det_fnv1a(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ull;            /* FNV-1a 64-bit prime */
    }
    return h;
}
uint64_t sim_state_hash(const SimState *s)
{
    uint64_t h = 14695981039346656037ull; /* FNV-1a 64-bit offset basis */
    if (s == NULL || s->chunk == NULL)
        return h;
    /* 0.5 M1 made Chunk.voxels a POINTER (was an inline array). `sizeof voxels`
     * is therefore the pointer WIDTH (8 on LP64 Linux, 4 on ILP32 Win32) - it
     * folded only the first 1-2 voxels, by a platform-dependent count, so the
     * voxel state (incl. water fill) was effectively unhashed and the hash was
     * not cross-platform comparable. Hash the FULL voxel array - or, for a
     * uniform-air chunk that borrows no slab (voxels==NULL), its uniform word. */
    if (s->chunk->voxels != NULL)
        h = det_fnv1a(h, s->chunk->voxels, (size_t)CHUNK_VOXELS * sizeof(Voxel));
    else
        h = det_fnv1a(h, &s->chunk->uniform_word, sizeof s->chunk->uniform_word);
    h = det_fnv1a(h, s->heat,          sizeof s->heat);
    h = det_fnv1a(h, s->latent,        sizeof s->latent);
    return h;
}
#endif /* VOXEL_DETERMINISM_HARNESS */
