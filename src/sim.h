/* sim.h - The cellular-automaton heat simulation: explicit FTCS diffusion on a
 * per-chunk active front, in fixed-point integers, on a fixed 15 Hz tick.
 *
 * Binding source: ARCHITECTURE.md Section 3 (the CA heart) and Section 2.2/2.3
 * (the 8-bit temperature codec and the MaterialDef thermal fields). This is the
 * heat-sim milestone EXTENDED with PHASE TRANSITIONS (ARCHITECTURE 3.5/3.8):
 * melting and solidifying driven by temperature crossing MaterialDef thresholds,
 * with LATENT-HEAT banking so a phase change absorbs/releases energy and takes
 * time. It implements heat diffusion + a held heat source + the active-front
 * wake/sleep machinery + the melt/freeze transition pass, and - as of the FLUID
 * milestone - CELLULAR FLUID FLOW for PHASE_LIQUID voxels (ARCHITECTURE Section
 * 3 fluid sim): liquids carry a 4-bit FILL column in the voxel `fill` nibble,
 * fall DOWNWARD into air/less-full same-material cells (gravity, never viscosity-
 * gated), then spread LATERALLY to equalise (gated by MaterialDef.viscosity), all
 * in conservative in-place integer transfers on the SAME active front. So molten
 * copper / lava (PHASE_LIQUID from M4 melting) and water now POOL and FLOW.
 *
 * DEFERRED to later milestones (flagged here so the contract stays honest):
 *   - boiling / evaporation (gas phase);
 *   - POWDERS (sand piling at rest_angle): a DIFFERENT motion rule (full-or-empty,
 *     down-diagonal at rest_angle, needs the 4 down-diagonal neighbours the strictly
 *     6-connected sim lacks) and its own determinism, so it is its own mini-
 *     milestone. The PHASE 1.6 dispatch is built to switch on MaterialDef.phase, so
 *     a future PHASE_POWDER -> powder_step slots into the same moved_mask / wake /
 *     dirty machinery this milestone establishes;
 *   - CROSS-CHUNK FLUID FLOW: like heat, the fluid pass treats out-of-chunk
 *     neighbours as a closed wall - flow that reaches a chunk face STOPS there.
 *     Deferred exactly as cross-chunk heat/seam is deferred (single-chunk sim);
 *   - FILL-HEIGHT TOP SURFACE rendering (a shorter top quad keyed on fill): liquids
 *     render as FULL translucent cubes this milestone; a sub-voxel surface level is
 *     a follow-up (it splits greedy merge per fill level and needs a fractional-Y
 *     vertex the 12-byte format does not produce - see mesher/render notes).
 *
 * This milestone is single-chunk; the sim sees one Chunk at a time and treats
 * out-of-chunk neighbours as a closed boundary (no flux / no flow), matching
 * light.c/mesher.c's per-chunk bring-up rule.
 *
 * WHAT THIS PASS OWNS, AND WHAT IT DOES NOT
 * -----------------------------------------
 * The sim owns the voxel TEMPERATURE field (bits 8..15, via vox_temp_code) and
 * the ACTIVE flag (VF_ACTIVE, bit 28). It ALSO writes the MATERIAL id (bits
 * 0..7) and the VF_LIQUID flag (bit 29) - during a phase transition (to the
 * target dictated by the MaterialDef melts_to/freezes_to data) AND during the
 * fluid pass (air<->liquid materialisation: an air cell that receives fill
 * becomes the flowing material with VF_LIQUID set; a liquid cell drained to
 * fill==0 reverts to MAT_AIR with VF_LIQUID cleared). Both are data-driven from
 * MaterialDef.phase/viscosity - never a switch on id (behaviour stays in the data
 * table, ARCHITECTURE 2.3). As of the fluid milestone the sim ALSO owns the FILL
 * nibble (bits 16..19) for PHASE_LIQUID voxels: the fluid pass moves fill between
 * cells conservatively. It NEVER touches a non-PHASE_LIQUID voxel's fill (solids
 * keep their fill=15 convention untouched), and still NEVER writes light/ao -
 * those belong to worldgen, light.c, and the mesher. The mesher READS temperature
 * at mesh time to derive a glow (Section 6 of the milestone task / viz below) and
 * READS fill/phase to split the opaque vs liquid mesh streams, but the sim never
 * writes the light field itself.
 *
 * =====================================================================
 * 1. FIXED-POINT SCHEME  (ARCHITECTURE 3.6 - "Fixed-Point, Not Floating-Point")
 * =====================================================================
 * NO float in the per-voxel hot loop (the Pentium M x87 path is too slow;
 * -mfpmath=sse governs only render-side float). The 8-bit two-segment voxel
 * temperature code is a STORAGE format only; the math runs at higher precision
 * while a voxel is awake, and re-quantizes to the 8-bit code only on commit.
 *
 *   Internal "heat unit" = degrees Celsius in Q<HEAT_FRAC_BITS> fixed point,
 *   i.e.  heat_units = celsius << HEAT_FRAC_BITS  (1/64 C resolution at 6 bits).
 *   A voxel's working temperature is  temp_to_heat(vox_temp_code(v))  -> int32.
 *   The world's full -40..2020 C range maps to -2560..129280 heat units, which
 *   sits comfortably in int32 with room for flux sums (see overflow note below).
 *
 * This wider accumulator is the mitigation for the 20 C/step industrial-band
 * stalling problem flagged in ARCHITECTURE 3.4: a slow conductor's sub-quantum
 * per-tick rise accumulates at 1/64 C resolution in the active-set scratch
 * across ticks, so it eventually crosses a code boundary rather than rounding to
 * zero every tick. (The prototype flag stands: whether this fully eliminates
 * visible stair-stepping in the glow view must still be confirmed on real M170
 * hardware before the 8-bit encoding is frozen.)
 *
 * The discrete update (ARCHITECTURE 3.4) for active voxel i with axis neighbours
 * j, all temperatures in heat units:
 *
 *     flux  = sum_j  kw[mat_i][mat_j] * (T_j - T_i)        // int64 accumulator
 *     dT    = flux >> HEAT_SHIFT                            // back to heat units
 *     T_i'  = clamp(T_i + dT)
 *
 * kw[a][b] is a precomputed per-material-pair integer weight in Q<HEAT_SHIFT>
 * fixed point that folds THREE physical quantities into one multiply:
 *     (a) the interface conductance k_ij = harmonic_mean(k_a, k_b)
 *           = 2*k_a*k_b / (k_a + k_b)   (the physically correct series-resistance
 *           combination, ARCHITECTURE 3.4 - a hot copper voxel against stone
 *           transfers at the rate the WORSE conductor allows: emergent insulation);
 *     (b) the inverse volumetric heat capacity of the RECEIVING voxel i,
 *           1 / C_a where C_a = density_a * specific_heat_a   (so the SAME face
 *           flux warms a low-heat-capacity voxel more than a high one - and the
 *           pair weight is deliberately ASYMMETRIC: kw[a][b] != kw[b][a] in
 *           general, because each side divides the face flux by its OWN C);
 *     (c) the stability normalization (see section 2 below).
 *
 * kw is built once at startup by sim_build_conduct_lut() from g_materials, into
 * a 256x256 int16 table (g_conduct_lut) = 65536 * 2 B = 128 KiB resident (the
 * ARCHITECTURE 3.4 "64K entries of int16 = 128 KiB" material-table budget line).
 *
 * OVERFLOW: a single term kw*(Tj-Ti) can reach ~21845 * 131840 ~= 2.9e9, past
 * int32; the six-term flux sum reaches ~1.7e10. The flux accumulator MUST be
 * int64. After  >> HEAT_SHIFT  the result dT is back inside int32 (bounded by
 * the max |Tj-Ti|, ~131840). This is a load-bearing correctness note.
 *
 * =====================================================================
 * 2. STABILITY  (ARCHITECTURE 3.4 - the von Neumann 1/6 bound, load-bearing)
 * =====================================================================
 * Explicit FTCS in 3D on the 6-connected lattice is conditionally stable: the
 * per-face diffusion weight w must satisfy  6 * w <= 1  (equivalently the
 * Fourier number alpha*dt/dx^2 <= 1/6). We normalize the unit system so the
 * MOST DIFFUSIVE material in g_materials (copper, alpha ~1.16e-4 m^2/s) sits at
 * the ceiling: its same-material per-face weight w = 1/6 exactly. Concretely
 * sim_build_conduct_lut() picks the timestep dt = (1/6) / alpha_max once, then
 * every kw[a][b] = round( dt * harmonic_mean(k_a,k_b) / C_a * 2^HEAT_SHIFT ).
 *
 * Two clamps make this provably safe for EVERY possible voxel configuration:
 *   - The diagonal kw[copper][copper] is CLAMPED to floor(2^HEAT_SHIFT / 6) so
 *     6*w is strictly < 1 (never the marginal == 1 case). All weights scale from
 *     this integer ceiling, so none can round above it.
 *   - For a dissimilar face, w[a][b] = dt * k_ab / C_a. Since the harmonic mean
 *     k_ab = 2 k_a k_b/(k_a+k_b) <= min(k_a,k_b) <= k_a, every dissimilar weight
 *     is <= the same-material weight w[a][a] <= 1/6. PROVEN: no neighbour pairing
 *     can exceed the single-material ceiling, so the worst-case row sum 6*w stays
 *     under 1 and nothing can blow up. This is THE correctness property.
 *
 * A consequence (correct, intended): very-low-diffusivity materials change
 * slowly per tick (stone w ~ 1/633, wood ~ 1/5140). Insulators are slow, which
 * is exactly why a stone cavity HOLDS heat. The smallest nonzero kw is still > 0
 * (verified) so no real material fully stalls - it just accumulates over ticks
 * in the wider internal accumulator.
 *
 * =====================================================================
 * 3. ACTIVE FRONT  (ARCHITECTURE 3.1/3.2 - we simulate fronts, not volumes)
 * =====================================================================
 * The sim touches only voxels on an active front, tracked per chunk as a compact
 * list of 12-bit local indices plus a presence bitmask (mirroring the light.c
 * BFS-ring scratch idiom and the ARCHITECTURE 3.2 ChunkActive sketch). A
 * uniform-temperature chunk has ZERO active voxels and costs nothing.
 *
 * WAKE: a voxel is woken when it can exchange meaningful heat with a neighbour -
 * specifically when |T_neighbour - T_self| exceeds SIM_WAKE_QUANTUM heat units
 * (one "quantum" of temperature difference worth propagating). The held source
 * (lava) is always awake. A heat_step that changes a voxel's temperature wakes
 * the neighbours it pushed more than SIM_WAKE_QUANTUM-equivalent flux into - the
 * propagation mechanism (the front advances one voxel-ring per tick).
 *
 * SLEEP: a voxel sleeps when, after a tick, its dT rounded to zero in heat units
 * AND it is within SIM_WAKE_QUANTUM of all six neighbours (locally equilibrated).
 * Sleep is LAZY (ARCHITECTURE 3.2): clear VF_ACTIVE now, physically drop the
 * index in the end-of-tick compaction pass, so active[] keeps a stable order
 * during the sweep and the double-buffer read/commit don't shuffle indices.
 *
 * DOUBLE-BUFFER (ARCHITECTURE 3.3): heat is double-buffered over the ACTIVE SET
 * ONLY, never the world. The read pass snapshots each active voxel's temperature
 * and computes its new temperature into a write list (HeatWrite, sized to the
 * active cap); a separate commit pass stores them. This makes the FTCS update a
 * true simultaneous step (every cell reads start-of-tick neighbour values), so
 * the result is independent of iteration order - no directional bias.
 *
 * =====================================================================
 * 4. TICK  (ARCHITECTURE 3.7 - fixed 15 Hz, decoupled from 30 FPS render)
 * =====================================================================
 * The sim runs at a fixed 15 Hz (66.67 ms period = one tick per ~2 render frames
 * at 30 FPS). main.c carries a millisecond accumulator: each frame it adds the
 * real frame dt, and while the accumulator >= SIM_TICK_MS it subtracts a tick's
 * worth and calls sim_tick() once. No catch-up spiral: at most a bounded number
 * of ticks run per frame; on overrun the in-world sim slows gracefully (active
 * voxels left over stay active for the next tick) while the frame never hitches.
 * The active cap (SIM_ACTIVE_CAP = 4096) is the last-resort throttle.
 *
 * DETERMINISM: heat is order-independent by construction (double-buffer, above).
 * Iteration order is fixed (per-chunk active[] order), so a replay from the same
 * state produces the same result.
 *
 * =====================================================================
 * 5. HEAT SOURCE  (ARCHITECTURE 3.4 - heat sources drive diffusion)
 * =====================================================================
 * For this milestone the heat source is the MAT_EMISSIVE lava voxel (the only
 * MAT_EMISSIVE starter material, also the block-light seed in light.c). It is a
 * Dirichlet boundary: held at SIM_LAVA_HOLD_C each tick (re-stamped to that code
 * after the commit pass and kept awake), so it continuously drives diffusion
 * into its neighbours rather than cooling down itself. SIM_LAVA_HOLD_C = 1150 C
 * (code 212, decodes 1160 C) - hot, above lava's own 1200 C solidus is NOT
 * required here since melt/freeze is deferred; 1150 C reads as a strong, stable
 * source that visibly warms a cavity. Any voxel can be designated a held source
 * via sim_set_source() for tests.
 *
 * =====================================================================
 * 6. VIZ  (milestone task Section 6 - see heat spread, no renderer surgery)
 * =====================================================================
 * Heat is made visible by folding a temperature-derived GLOW into the per-face
 * light byte AT MESH TIME. The mesher PACKS the glow into the HIGH nibble of the
 * light byte and the baked sky/block light into the LOW nibble (mesher.c), and
 * the byte is uploaded NON-normalized (raw 0..255). The shader SPLITS the two
 * nibbles: the low nibble feeds the sun-scaled diffuse term (so a moving sun
 * folds in live, no remesh), and the high nibble drives a varying v_heat that
 * the fragment shader turns into a temperature-derived EMISSIVE and ADDS on top
 * of the diffuse term. That emissive is MATERIAL-BASE-TINTED (render.c blends
 * the hot glow toward the voxel's own base colour), so white-hot molten copper
 * glows copper-orange while lava glows red-orange - distinct, and data-driven
 * (no material-id switch). A heat plume thus reads as a spreading glowing region.
 * The mesher derives the glow level from the voxel temperature code with the
 * integer ramp sim_temp_glow() below. main.c remeshes a chunk when sim_tick()
 * marks it dirty (CHUNK_DIRTY_MESH), re-running light_compute() then
 * greedy_mesh() then re-uploading - the existing dirty path. (The sun and its
 * tint are live shader uniforms, so a day/night cycle needs no remesh at all.)
 *
 * =====================================================================
 * 7. PHASE TRANSITIONS  (ARCHITECTURE 3.5 / 3.8 - melt/freeze + latent heat)
 * =====================================================================
 * A new PHASE 1.5 transition sub-step runs in sim_tick AFTER the PHASE 2 heat
 * commit (and source re-stamp) and BEFORE PHASE 3 compaction (ARCHITECTURE 3.7
 * "heat first, then transitions, against the freshly-committed temperatures").
 * It iterates the active front, skipping held sources, and per voxel evaluates
 * a melt and a freeze branch GATED PURELY BY MaterialDef DATA (melt_point_c,
 * melts_to, freezes_to, phase, latent_fusion) - no material id is named in the
 * sim (ARCHITECTURE 2.3, the data-driven rule): a voxel melts iff its
 * melt_point_c >= 0 AND melts_to != self; a liquid freezes iff freezes_to !=
 * self AND its (molten) melt_point_c >= 0, the freeze threshold being that same
 * melt_point_c (solidus == liquidus).
 *
 * LATENT-HEAT CURRENCY (binding): latent heat is banked in the SAME internal
 * heat-unit currency as the FTCS loop (Celsius << HEAT_FRAC_BITS, 1/64 C). The
 * energy to fully change one voxel is expressed as an equivalent count of heat
 * units of the material's OWN sensible heat:
 *
 *     latent_units = latent_fusion[kJ/kg] * 1000 * HEAT_ONE_C / specific_heat
 *
 * (latent_fusion*1000 / specific_heat = degrees C of sensible heat the material
 * would otherwise absorb; times HEAT_ONE_C puts it in heat units). FLOAT-FREE,
 * all intermediates < 2.6e7 so int32 holds them with headroom; precomputed ONCE
 * into the process-global side LUT g_latent_units[] inside sim_build_conduct_lut
 * and read through sim_latent_units() so the hot loop never divides. Scaling by
 * the material's OWN specific_heat makes banking commensurate with temp_to_heat
 * ("the voxel sat at the melt point and absorbed N heat units it could not turn
 * into temperature"), which is exactly what the energy-accounting tests check.
 *
 * BANKING (melt): on the tick a voxel's committed temp first reaches/exceeds its
 * melt point, start banking: latent[li] += (T_heat - melt_point_heat) and the
 * stored temp is CLAMPED back to the melt-point code so the temperature PLATEAUS
 * while banking. Each later tick the FTCS loop pushes more heat in; the over-
 * threshold amount is banked again and re-clamped. When latent[li] reaches the
 * material's latent_units the id is swapped to melts_to, VF_LIQUID is set, the
 * banked EXCESS above latent_units is spilled into the new molten temp, latent
 * is freed, and the voxel + its 6 face neighbours are re-woken. This makes
 * melting TAKE TIME and CONSERVES ENERGY (every unit pushed in raised temp to
 * the plateau, sits banked, or spilled as excess - nothing discarded). FREEZE is
 * the exact reverse: a molten voxel below its freeze point banks the heat DEFICIT
 * it must shed (holding temp clamped UP at the freeze point) until latent_units
 * is reached, then swaps to freezes_to, clears VF_LIQUID, sets temp below freeze.
 *
 * Held Dirichlet sources are skipped entirely (a re-stamped boundary must never
 * melt or freeze itself); lava is such a source so its freeze branch is dormant.
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include "chunk.h"
#include "voxel.h"
#include "progress.h"   /* ProgressSink (the optional, borrowed event ring) +
                         * ProgressEvent / prog_emit - the read-only progression
                         * observer's sim->observer channel (see SimState.progress
                         * and sim_set_progress_sink below; ARCHITECTURE Section 9). */

/* ---- Fixed-point scheme constants (binding) ------------------------------ */
/* Internal temperature is Celsius in Q HEAT_FRAC_BITS fixed point. 6 bits ->
 * 1/64 C resolution: fine enough that slow conductors accumulate sub-quantum
 * rises across ticks rather than rounding to zero (ARCHITECTURE 3.6). */
#define HEAT_FRAC_BITS   6
#define HEAT_ONE_C       (1 << HEAT_FRAC_BITS)   /* heat units per 1 C (= 64) */

/* The conductance LUT fixed-point shift. Chosen so the copper ceiling weight
 * floor(2^HEAT_SHIFT / 6) fits a signed 16-bit LUT entry (21845 < 32767) - this
 * is what keeps g_conduct_lut at the ARCHITECTURE 3.4 128 KiB int16 budget. */
#define HEAT_SHIFT       17

/* ---- Active-front constants (binding, ARCHITECTURE 3.1/3.2/3.7) ---------- */
/* Hard per-tick active-voxel cap: the load-bearing throttle (ARCHITECTURE 3.7).
 * 4096 = one chunk's worth; the 7-neighbour read working set is 112 KiB, L2
 * resident. On overrun the sim slows in-world, never drops the frame. */
#define SIM_ACTIVE_CAP   4096u

/* Per-chunk active list capacity. A single 16^3 chunk has at most 4096 voxels,
 * and for this single-chunk milestone the whole front lives in one chunk, so the
 * list is sized to the full chunk (matches CHUNK_VOXELS). The presence bitmask
 * is 4096 bits = 512 bytes (ARCHITECTURE 3.2 in_active_mask). */
#define SIM_CHUNK_ACTIVE_MAX  CHUNK_VOXELS       /* 4096 */
#define SIM_ACTIVE_MASK_WORDS (CHUNK_VOXELS / 32)/* 128 u32 = 4096 bits = 512 B */

/* Wake/sleep threshold in heat units: a neighbour temperature difference at or
 * below this is "equilibrated" (no wake, candidate to sleep). 1 C of difference
 * is the natural quantum - below it nothing visible propagates. In heat units
 * that is HEAT_ONE_C. */
#define SIM_WAKE_QUANTUM HEAT_ONE_C              /* 1 C worth of difference */

/* ---- Tick scheduling (binding, ARCHITECTURE 3.7) ------------------------- */
#define SIM_TICK_HZ      15
#define SIM_TICK_MS      (1000.0 / (double)SIM_TICK_HZ)  /* 66.666... ms */
/* Cap on ticks run in one frame so a long stall can never trigger a catch-up
 * spiral (ARCHITECTURE 3.7 "no catch-up"); main.c clamps the accumulator too. */
#define SIM_MAX_TICKS_PER_FRAME 4

/* ---- Heat source (binding, ARCHITECTURE 3.4) ----------------------------- */
/* The held temperature of a Dirichlet heat source (lava). Re-stamped each tick
 * so the source never cools and continuously drives diffusion. */
#define SIM_LAVA_HOLD_C  1150

/* ---- Fluid-flow constants (binding, ARCHITECTURE Section 3 fluid sim) ----- *
 * A PHASE_LIQUID voxel's water column is the 4-bit `fill` nibble (bits 16..19,
 * vox_fill/vox_set_fill), range 1..15 when present; 15 = brim-full. The
 * conserved quantity is sum(fill) over all liquid voxels of a given material.
 * fill==0 + a liquid material is the transient "drained" state the fluid pass
 * normalises by reverting the voxel to MAT_AIR (fill 0, VF_LIQUID cleared). */
#define FLUID_FULL          15u   /* brim-full fill; the solid fill convention too */

/* Lateral anti-oscillation gate: a neighbour receives only when the level gap is
 * at least this (so xfer = gap/2 >= 1). A 1-level difference is "settled" and
 * never moves - this is what makes the discrete relaxation monotone-decreasing in
 * the max-min spread, so it provably terminates at the level pool with no ringing
 * (halving the gap kills the 2-cell oscillation "move 1 toward average" suffers). */
#define FLUID_SETTLE_GAP    2u

/* Lateral STEP BUDGET per tick, keyed on MaterialDef.viscosity (the only place
 * viscosity gates flow; gravity/down is never viscosity-gated so even lava falls
 * promptly and only oozes sideways). Water (visc 0) dumps its whole lateral excess
 * in one tick; any positive viscosity bleeds one level per acting tick. */
#define FLUID_STEP_WATER    15u   /* visc == 0: effectively unbounded lateral push */
#define FLUID_STEP_VISCOUS  1u    /* visc  > 0: at most one level sideways per tick */

/* CADENCE GATE period bounds (the "only every Nth tick" ooze). The per-material
 * period is a power of two derived from viscosity with a pure shift, baked into
 * the g_visc_period[256] side LUT in sim_build_conduct_lut and read through
 * sim_visc_period(): period = clamp(1 << (visc >> 6), 1, 8). So water (visc 0)
 * acts every tick (period 1); molten_copper (200) every 4th; molten_iron (230)
 * and lava (250) every 8th. A cell spreads laterally only when
 * (tick_index & (period - 1)) == 0; gravity ignores the gate entirely. */
#define FLUID_PERIOD_MIN    1u
#define FLUID_PERIOD_MAX    8u

/* ---- Communicating vessels (fluid milestone 0.2: pressure lift + finisher) ---*
 * The gravity+lateral rule levels a SINGLE free surface but cannot make a column
 * RISE against gravity through a bottom channel (water in tank A reaching tank B
 * from below). Two cooperating pieces fix that (the connected-body finisher is
 * "Approach B", validated in the 2-D prototype wf_finisher.c; the gradual pressure
 * lift it was paired with proved unstable in 3-D and was dropped - the finisher
 * does the lift as a snap):
 *
 *  1. A no-mass HEAD field (head[] side array, like heat[]/latent[]): per cell, the
 *     MAX free-surface level of its connected same-material body, recomputed FROM
 *     SCRATCH each tick by a per-body flood. sim_liquid_unsettled keeps a cell whose
 *     body has a taller surface elsewhere (more than FLUID_UP_MARGIN above its own
 *     column) AWAKE, so an un-levelled communicating body does not sleep before the
 *     finisher levels it. (An incremental relaxation was tried first; its stored
 *     standing wave let a drained column's old height circulate as a "ghost" and
 *     momentarily collapse the field, and the sleep guard latched that dip and slept
 *     the body permanently un-levelled - hence the from-scratch flood.)
 *
 *  2. A connected-body FINISHER: when a body is STUCK (its fill-state hash recurs in
 *     a small ring - whether a static stall or a sub-cell limit cycle), ONE bounded
 *     non-local sweep flood-fills each connected body and rewrites every column to
 *     the SAME flat surface, conserving the body's total EXACTLY (skipping any
 *     interior solid). This is what raises the second tank, as a snap. Fired rarely
 *     (once per body per disturbance), then the body is a fixed point and SLEEPS. */
#define FLUID_UP_MARGIN     FLUID_FULL  /* a FULL cell (matches prototype UP_MARGIN=MAXFILL):
                                   * lift/keep-awake only toward a surface >= one whole cell
                                   * higher. A sub-cell margin (e.g. 2) fires the rise/sleep
                                   * clause on the within-1 roughness of an ordinary flat
                                   * puddle and pins it awake forever (broke "still pond
                                   * costs nothing"). */
#define FLUID_RING_N        16    /* recent-fill-hash ring for stuck-body detection */
/* Consecutive ticks the fill-state hash must RECUR before the finisher judges a
 * body stuck and fires. MUST exceed FLUID_PERIOD_MAX: a VISCOUS liquid (molten
 * metal) spreads on a slow lateral cadence, so its fill state is constant for up to
 * FLUID_PERIOD_MAX-1 ticks BETWEEN cadence steps while still legitimately flowing.
 * A smaller confirm window (e.g. 4) mistook those pauses for "stuck" and fired the
 * snap mid-spread, teleporting a still-spreading ooze and re-waking large rings.
 * (FLUID_PERIOD_MAX+4) clears the longest legitimate pause with margin; water
 * (period 1) is unaffected - a genuinely stuck pool recurs every tick. */
#define FLUID_CYCLE_CONFIRM (FLUID_PERIOD_MAX + 4u)  /* > FLUID_PERIOD_MAX (=12)     */
#define FLUID_FIRED_MAX     32    /* distinct snapped equilibria remembered (O(1))  */

/* ---- Viz glow ramp (milestone task Section 6) ---------------------------- */
/* Temperature-code window mapped to a 0..15 glow level the mesher packs into the
 * high nibble of the face light byte. Below GLOW_LO_CODE: no glow (ambient). At/above
 * GLOW_HI_CODE: full glow. Codes are the binding two-segment codec (voxel.h):
 *   GLOW_LO_CODE = temp_encode_c(60)   -> 100   (heat becomes visible ~60 C)
 *   GLOW_HI_CODE = temp_encode_c(1000) -> 204   (saturates by ~1000 C)
 * Pinned as literals so the mesher needs no float at mesh time. */
#define SIM_GLOW_LO_CODE 100u
#define SIM_GLOW_HI_CODE 204u

/* ---- Per-chunk active tracking (ARCHITECTURE 3.2 ChunkActive) ------------ *
 * The acceleration structure over the per-voxel VF_ACTIVE ground truth: a
 * compact list of 12-bit local indices the sim walks linearly (cache-friendly),
 * plus a 4096-bit presence mask so a voxel woken by several neighbours in one
 * tick is enqueued once. wake/sleep own the invariant that list and mask agree
 * with the VF_ACTIVE flag. Lives in SimState, keyed alongside the Chunk pointer
 * (this milestone is single-chunk; multi-chunk worklists are a later milestone). */
typedef struct {
    uint16_t active[SIM_CHUNK_ACTIVE_MAX];        /* packed local indices 0..4095 */
    uint16_t count;                               /* live entries                 */
    uint32_t in_active_mask[SIM_ACTIVE_MASK_WORDS];/* 4096 bits: is idx queued?   */
    uint8_t  overflow;                            /* set if a wake hit the cap    */
} ChunkActive;

/* One pending temperature write, double-buffering the active front (not the
 * world). Read pass fills these; commit pass stores t_new in place. Mirrors the
 * ARCHITECTURE 3.3 HeatWrite { Chunk*, li, t_new } sketch, EXTENDED to carry the
 * FULL-PRECISION internal temperature (t_new_heat, in Q HEAT_FRAC_BITS heat
 * units) alongside the 8-bit code: the precise value is the authoritative one
 * committed into SimState.heat[] (so sub-quantum per-tick rises ACCUMULATE
 * across ticks instead of rounding to zero every commit - the ARCHITECTURE 3.4
 * "heat accumulates at full internal resolution" mitigation). t_new is the
 * derived storage/mesh/glow code, recomputed at commit from t_new_heat. */
typedef struct {
    uint16_t li;          /* local index within the chunk                        */
    uint8_t  t_new;       /* re-quantized 8-bit temperature code (storage/mesh)  */
    int32_t  t_new_heat;  /* precise internal temperature in heat units (truth)  */
} HeatWrite;

/* ---- Held heat source descriptor (Dirichlet boundary) -------------------- */
/* A voxel held at a fixed temperature each tick. For the milestone the lava
 * voxel is registered automatically by sim_init() (it scans for MAT_EMISSIVE);
 * tests register their own via sim_set_source(). */
typedef struct {
    uint16_t li;        /* local index of the held voxel                         */
    uint8_t  hold_code; /* the temperature code to re-stamp each tick            */
    uint8_t  active;    /* 1 = this slot is a live source                        */
    uint8_t  is_spring; /* 1 = ALSO re-fill to brim each tick (inexhaustible      *
                         *     PHASE_LIQUID spring); 0 = HOLD HEAT ONLY - a hot   *
                         *     pool whose fill is CONSERVED, so it does not flood.*
                         *     The held-heat vs held-fill decoupling: lava is     *
                         *     heat-only (set via sim_set_source); a deliberate   *
                         *     spring uses sim_set_spring.                        */
} HeatSource;

#define SIM_MAX_SOURCES  16

/* ---- The sim state ------------------------------------------------------- *
 * Single-chunk for this milestone: one ChunkActive over the one Chunk, the
 * double-buffer write scratch, and the held-source list. Allocated once by
 * sim_init(), reused across ticks, freed by sim_shutdown(). The conductance LUT
 * is process-global (built once, const after build) so it is shared and not
 * carried per SimState. */
/* 0.5 M4 cross-chunk water deposit callback (see SimState.fluid_xfn). Forward-
 * declared with the struct tag so the field below can name the typedef. */
struct SimState;
typedef int  (*SimXFlowFn)(void *user, const struct SimState *s, int src_li,
                           int face, int nlx, int nly, int nlz);
typedef struct SimState {
    Chunk      *chunk;                  /* the single simulated chunk            */
    ChunkActive act;                    /* its active front                      */
    HeatWrite   writes[SIM_ACTIVE_CAP]; /* per-tick double-buffer (active set)   */
    HeatSource  sources[SIM_MAX_SOURCES];
    uint16_t    n_sources;
    uint64_t    tick_index;             /* monotone tick counter (determinism)   */
    uint32_t    n_writes;               /* 0.4 M4: pending writes handed READ->COMMIT */
    uint8_t     dirty_mesh;             /* set when a tick changed visible state; *
                                         * main.c reads + clears it to remesh    */
    /* ---- Fluid MOVED-THIS-TICK guard (fluid milestone, ARCHITECTURE 3.3) ---- *
     * A per-tick 4096-bit presence mask (128 u32 = 512 B), cleared at the top of
     * the PHASE 1.6 fluid pass. When a cell donates or receives fill it is marked
     * moved; a cell already marked moved is SKIPPED when the in-place sweep
     * reaches it. This is the classic falling-sand aliasing fix: it stops a cell
     * that just received fill from being re-processed and double-moving in the
     * same tick. Mass transport is in-place (the doc forbids double-buffering it -
     * a simultaneous fill update aliases mass into one cell and destroys
     * conservation), so this guard + integer transfers + conservation-by-
     * construction (every unit subtracted from one cell is added to exactly one
     * neighbour) make a full fluid run byte-for-byte reproducible from a given
     * world + tick_index. Lives SimState-side (NOT a voxel flag - the 4-bit flag
     * nibble ACTIVE/LIQUID/DIRTY_MESH/RESERVED is full), mirroring the
     * ChunkActive in_active_mask layout. Single-chunk this milestone. */
    uint32_t    moved_mask[SIM_ACTIVE_MASK_WORDS];
    /* ---- Latent-heat accumulator (ARCHITECTURE 3.8 side structure) -------- *
     * Banked latent heat per local index, in the SAME heat-unit currency as the
     * FTCS loop (Celsius << HEAT_FRAC_BITS). The voxel word has NO spare field
     * for transient phase-change energy, so it lives HERE in a side structure,
     * never the voxel (binding). Convention: latent[li] == 0 means "no
     * transition in progress"; > 0 means "banking, value = heat units
     * accumulated toward sim_latent_units(mat)". Single-chunk this milestone, so
     * a flat CHUNK_VOXELS array (4096 * 4 = 16 KiB) is the simplest correct,
     * branch-free, cache-local form (a hash sized to the active cap is the
     * multi-chunk form for later). Zeroed by sim_init; a voxel with a live entry
     * is kept awake (it must not sleep mid-plateau). */
    int32_t     latent[CHUNK_VOXELS];
    /* ---- Full-resolution internal temperature (ARCHITECTURE 3.4 mitigation) -*
     * The AUTHORITATIVE per-voxel temperature in internal heat units (Celsius <<
     * HEAT_FRAC_BITS, 1/64 C), mirroring latent[] as a SimState-side array. The
     * 8-bit voxel temp code (voxel.h) is only the STORAGE / mesh / glow view; it
     * is re-derived from heat[] on commit. Persisting full resolution HERE is the
     * fix for the ARCHITECTURE 3.4 "20 C/step quantization stall": a slow
     * conductor's sub-quantum per-tick rise accumulates at 1/64 C in heat[]
     * across ticks instead of rounding to zero when re-quantized to the code
     * every tick. heat[] is kept VALID FOR ALL VOXELS AT ALL TIMES: sim_init
     * seeds the whole array from each voxel's code, sim_set_source and the
     * per-tick source re-stamp keep held voxels in sync, and every commit goes
     * THROUGH heat[] - so a freshly-woken voxel already has a correct heat value
     * and wake needs no special initialisation. Single-chunk this milestone, so
     * a flat CHUNK_VOXELS array (4096 * 4 = 16 KiB) matches latent[]. */
    int32_t     heat[CHUNK_VOXELS];
    /* ---- Communicating-vessels HEAD field (fluid milestone 0.2) ------------- *
     * A no-mass side structure (like latent[]/heat[]): head[li] is the MAX free-
     * surface level (sub-cell units, ly*FLUID_FULL + fill) anywhere in the connected
     * same-material liquid body that li belongs to. head_relax() recomputes it FROM
     * SCRATCH each tick by flooding every body (no stored state, so no stale "ghost"
     * value can circulate and momentarily collapse the field). sim_liquid_unsettled
     * reads head to keep a cell whose body has a TALLER surface elsewhere (a second
     * tank still below a full first tank joined by a bottom channel) AWAKE until the
     * connected-body finisher levels it. NOT persisted (transient, like the active
     * front): zeroed by sim_init, rebuilt by the flood, reset on edit. A drained/air
     * cell, or any held-source cell, carries head 0. Single-chunk this milestone. */
    int32_t     head[CHUNK_VOXELS];
    /* ---- Connected-body FINISHER limit-cycle memory (fluid milestone 0.2) --- *
     * The local rule can enter a short limit cycle (sub-cell slosh) it never
     * settles out of when the flat equilibrium falls between integer cell levels.
     * fluid_ring holds the last FLUID_RING_N fill-state hashes; when the current
     * hash recurs for FLUID_CYCLE_CONFIRM consecutive ticks the body is judged
     * macroscopically settled with only sub-cell slosh, and ONE connected-body
     * flat-snap fires. fluid_fired remembers already-snapped equilibria so we never
     * re-fire for a state we already levelled (keeps it O(1) fires per disturbance).
     * All transient: zeroed by sim_init, reset by sim_notify_edit and on each fire.
     * Hashing/trigger run ONLY when liquid is active, so a still pond costs nothing.*/
    uint64_t    fluid_ring[FLUID_RING_N];
    uint64_t    fluid_fired[FLUID_FIRED_MAX];
    uint16_t    fluid_ring_fill;        /* live entries in the ring (<= RING_N)     */
    uint16_t    fluid_ring_pos;         /* next write slot (mod RING_N)             */
    uint16_t    fluid_cyc_seen;         /* consecutive in-cycle ticks               */
    uint16_t    fluid_n_fired;          /* distinct snapped equilibria remembered   */

    /* ---- 0.5 fluid: binary-fill flow + radial gravity (M3) ---- *
     * fluid_down is the SIM_NEIGH face index (0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z) that
     * points "down" (radially toward the planet centre) for THIS chunk - cached
     * per-chunk by main.c from the chunk centre vs WG_PLANET_C*. Default 3 (-Y), the
     * flat-gravity fallback the tests + the proto use. Binary water (fill {0,15})
     * moves WHOLE voxels along this face + flow-to-descent laterally.
     * fluid_finisher_on gates the partial-fill head/connected-body finisher (the
     * world-Y column solver): OFF in M3 (the binary flow rule settles on its own,
     * terraced); M4 replaces the finisher with a radial shell-snap and turns it on. */
    int8_t      fluid_down;             /* radial-down SIM_NEIGH face (default 3 = -Y) */
    uint8_t     fluid_finisher_on;      /* 0 in M3 (terraced); M4 enables the snap    */
    /* 0.5 M4: cross-chunk water flow. NULL on a single-chunk sim (closed wall, M3);
     * the WorldCA sets these each tick so a river can fall down across chunk seams.
     * Transient (a per-tick callback into the WorldCA's tick context) - NOT persisted
     * state and carries nothing the determinism/replay contract depends on. */
    SimXFlowFn  fluid_xfn;
    void       *fluid_xfn_user;
    /* ---- OPTIONAL progression event sink (ARCHITECTURE Section 9, READ-ONLY) -*
     * A single BORROWED pointer to a ProgressSink (a ProgressRing) the sim only
     * ever PUSHES to, on emergent transitions it ALREADY computes (a melt/freeze
     * completes; a committed temp code first crosses a tier band; optionally a
     * liquid first pools). This is the ONLY sim->observer channel.
     *
     * THE BINDING INVARIANT this field exists to honor: the progression layer is
     * a read-only OBSERVER. Remove it and the world simulates BYTE-IDENTICALLY.
     * Guaranteed structurally HERE by being OPTIONAL and NULLABLE:
     *   - DEFAULT IS NULL. The field starts NULL (a zero-initialised SimState, or
     *     a clear in sim_init) and is installed only via sim_set_progress_sink
     *     AFTER init, so a sim with no setter call emits NOTHING. The emit hook in
     *     sim.c is `if (s->progress) { build ev; prog_emit(s->progress, &ev); }` - when
     *     NULL the sim does not even BUILD the event, so it is provably a no-op:
     *     NULL sink => zero observation => byte-identical sim. This is also HOW
     *     the read-only invariant is tested (test_progress.c runs the same world
     *     WITH a sink vs WITH NULL and memcmp's the voxel array at every tick).
     *   - BORROWED, NOT OWNED. The ring is owned by the caller (main.c / a test),
     *     passed by pointer via sim_set_progress_sink(). SimState frees nothing
     *     and the ring is NOT part of SimState's identity: it carries no state the
     *     sim persists or that affects determinism, so the persistence and replay
     *     contracts are untouched and the existing sim tests still pass (the field
     *     defaults NULL and sim_init does not seed it from any input).
     *   - SINGLE-PRODUCER / SINGLE-CONSUMER. The sim only ever APPENDS via
     *     prog_emit (O(1), float-free, no alloc, no callback); it NEVER reads the
     *     ring back, so no sim decision can depend on its contents. The observer
     *     (main.c, off the sim budget) is the sole consumer via prog_observe_drain.
     *
     * This is the ONLY field added to SimState for the progression milestone. */
    ProgressSink *progress;             /* borrowed; NULL = no observation        */
} SimState;

/* ===================================================================== *
 *  API  - minimal, matched to chunk.h. Pure C, no GL / no OS.           *
 * ===================================================================== */

/* Build the 256x256 int16 conductance LUT (g_conduct_lut) from g_materials,
 * normalized to the 1/6 stability ceiling (section 2 above). ALSO precomputes
 * the process-global latent-heat LUT g_latent_units[] (section 7 above) by the
 * float-free integer formula. Idempotent; safe to call once at startup before
 * any sim_init(). Pure integer table build. */
void sim_build_conduct_lut(void);

/* Read the precomputed latent-energy bank target for a material id, in internal
 * heat units (the count of heat units a voxel must accumulate at its melt/freeze
 * plateau before the phase actually flips). Returns 0 for a material with no
 * latent_fusion or specific_heat == 0. A thin accessor over the g_latent_units[]
 * table built by sim_build_conduct_lut(), exposed so tests can assert the
 * expected plateau energy without re-deriving the formula. */
int32_t sim_latent_units(uint8_t mat);

/* Read the precomputed lateral CADENCE PERIOD for a material id: the power-of-two
 * tick stride at which a cell of this material is allowed to spread laterally
 * (1 = every tick / water, up to FLUID_PERIOD_MAX = 8 for lava). A cell spreads
 * only when (tick_index & (sim_visc_period(mat) - 1)) == 0; gravity ignores it.
 * A thin accessor over the g_visc_period[256] side LUT built by
 * sim_build_conduct_lut(), exposed so tests can assert the viscosity cadence
 * without re-deriving the float-free shift. Returns FLUID_PERIOD_MIN (1) for a
 * non-PHASE_LIQUID material (it never spreads, so the period is moot). */
uint8_t sim_visc_period(uint8_t mat);

/* Predicate read by PHASE 1's heat-sleep guard: true iff local index li is a
 * PHASE_LIQUID voxel that can still flow this tick - i.e. it can fall (the cell
 * below is air or same-material with fill < 15) OR it has a same-material
 * horizontal face neighbour more than 1 fill level away. Reads ONLY fill /
 * neighbour-fill + MaterialDef.phase, NEVER the temperature buffer, so heat
 * determinism is untouched; it only affects whether VF_ACTIVE is cleared
 * (front membership), keeping an unsettled liquid awake so PHASE 1.6 sees it.
 * A settled level pool fails the predicate and sleeps normally -> zero cost
 * ("a still pond costs nothing"). Out-of-chunk neighbours are a closed wall. */
int  sim_liquid_unsettled(const SimState *s, uint16_t li);

/* Bind the sim to one chunk. Zeroes the active front, scans the chunk for
 * MAT_EMISSIVE voxels and registers them as held heat sources, and seeds the
 * active front from every voxel that differs from a neighbour by more than
 * SIM_WAKE_QUANTUM (so a freshly-placed hot source wakes immediately). Calls
 * sim_build_conduct_lut() if it has not been built. Returns 0 on success. */
int  sim_init(SimState *s, Chunk *c);

/* Run exactly one 15 Hz heat tick (ARCHITECTURE 3.7 sim_tick shape):
 *   PHASE 1 (read, double-buffered): for each active voxel up to SIM_ACTIVE_CAP,
 *           compute its new temperature from start-of-tick neighbour temps into
 *           s->writes; wake neighbours it pushes > SIM_WAKE_QUANTUM flux into. A
 *           voxel with a live latent entry (latent[li] != 0) is held awake (it
 *           must not sleep mid-plateau).
 *   PHASE 2 (commit): store every queued t_new in place; re-stamp held sources.
 *           EXTENDED for fluids: a held source whose material is PHASE_LIQUID is
 *           ALSO re-stamped to fill=FLUID_FULL each tick (held full, the mass
 *           analogue of the temperature Dirichlet boundary), so it floods outward
 *           without being drained.
 *   PHASE 1.5 (transitions): against the freshly-committed temperatures, walk
 *           the active front, skip held sources, and run the melt/freeze latent-
 *           heat banking per voxel (try_phase_change in sim.c). Reads ONLY
 *           MaterialDef thresholds (melt_point_c, melts_to, freezes_to, phase,
 *           latent_fusion) - NO switch on material id. A completed transition
 *           swaps the voxel material id and VF_LIQUID, STAMPS fill=FLUID_FULL on
 *           the new phase (a melted/solidified cubic metre is full - the seam that
 *           lets a just-melted voxel flow in PHASE 1.6 without self-reverting to
 *           air), and wakes its neighbour ring; a visible change (dirty_mesh).
 *   PHASE 1.6 (FLUID FLOW, fluid milestone): placed AFTER 1.5 (so a freshly-
 *           melted PHASE_LIQUID voxel flows in the SAME machinery) and BEFORE
 *           PHASE 3. Clears s->moved_mask; snapshots fluid_count = act->count;
 *           for each active li in fixed active[] order, skips non-PHASE_LIQUID
 *           voxels (read MaterialDef.phase) and runs fluid_step(s, li): GRAVITY
 *           first (full downward transfer into air / same-material < 15, never
 *           viscosity-gated), then LATERAL equalisation of the residue (gap >=
 *           FLUID_SETTLE_GAP, xfer = gap/2, capped by the per-material step budget
 *           and the cadence gate keyed on sim_visc_period(mat) and tick_index).
 *           Determinism: in-place conservative integer transfers, the moved_mask
 *           guard, and PARITY-ALTERNATING lateral visit order keyed on tick_index
 *           (even: +X,-X,+Z,-Z; odd: the X/Z pairs reversed) kill directional bias
 *           with no PRNG. A held source DONATES but is never drained. A drained
 *           cell (fill->0) reverts to MAT_AIR (VF_LIQUID cleared). Any fill change
 *           or air<->liquid materialisation sets dirty_mesh (main.c remeshes).
 *           Single-chunk: out-of-chunk neighbours are a closed wall (no flow).
 *   PHASE 3 (compaction): drop lazily-slept indices, keeping active[] order. The
 *           heat-sleep guard in PHASE 1 is gated by !sim_liquid_unsettled() so an
 *           unsettled liquid stays on the front for PHASE 1.6; a settled pool
 *           sleeps and costs nothing.
 * Sets s->dirty_mesh if any committed temperature, material/phase, OR fill
 * changed (so main.c remeshes). Order-independent for heat (double-buffer) and
 * deterministic for fluids (in-place + moved_mask + parity sweep). */
void sim_tick(SimState *s);

/* ---- 0.4 M4: split tick for the world-wide cross-chunk CA ---------------- *
 * The per-chunk tick is split into a READ pass (PHASE 1: compute the write list
 * from START-OF-TICK state) and a COMMIT pass (PHASE 2 commit + 1.5 transitions
 * + 1.6 fluid + 3 compaction). The WorldCA runs ALL active chunks' READ before
 * ANY commit, so a cross-chunk boundary read sees the neighbour's start-of-tick
 * state (order-independent; the seam diffuses exactly like an interior face).
 *
 *   nfn (nullable): supply a cross-chunk neighbour voxel's start-of-tick heat +
 *     material for FACE (0..5, Face-enum / neigh[] order) at the neighbour's
 *     LOCAL coords (nlx,nly,nlz). Return 1 if a neighbour exists, 0 = closed
 *     wall (no flux). NULL nfn => every out-of-chunk face is a closed wall, which
 *     is the 0.3 single-chunk behaviour (byte-identical).
 *   wfn (nullable): request waking the neighbour CHUNK when this voxel pushed a
 *     quantum of flux across the seam (enqueue-only; the WorldCA acts on it after
 *     the commit pass, so the woken chunk first participates NEXT tick).
 *
 * sim_tick(s) is now exactly: sim_tick_ex(s, READ|COMMIT, NULL,NULL,NULL) then
 * ++tick_index - the single-chunk closed-wall path, unchanged for every existing
 * caller (tests, the warm-up soak). The WorldCA feeds tick_index itself and calls
 * the two phases separately with its cross-chunk nfn/wfn. */
typedef int  (*SimNeighFn)(void *user, const SimState *s, int face,
                           int nlx, int nly, int nlz,
                           int32_t *out_heat, uint8_t *out_mat);
typedef void (*SimWakeFn)(void *user, const SimState *s, int face,
                          int nlx, int nly, int nlz);
/* SimXFlowFn (0.5 M4 cross-chunk water deposit) is declared above, before the
 * SimState struct, since a SimState field holds one. */
#define SIM_PHASE_READ    1
#define SIM_PHASE_COMMIT  2
void sim_tick_ex(SimState *s, int phases, SimNeighFn nfn, SimWakeFn wfn, void *user);

/* Notify the sim that a PLAYER EDIT changed voxel li in the bound chunk (a block
 * broken to air, or one placed). Re-seeds heat[li] from the voxel's current temp,
 * clears its latent bank, and wakes li + its 6 face neighbours so heat/fluid
 * re-evaluate around the edit. Call AFTER mutating the voxel, and ONLY when the
 * edited chunk is this SimState's bound chunk (s->chunk). No-op if unbound or li
 * is out of range. li is a local 0..CHUNK_VOXELS-1 index (vox_index). */
void sim_notify_edit(SimState *s, int li);

/* Register a held Dirichlet heat source at local index li, held at hold_code
 * each tick (the source never cools, continuously driving diffusion). Used by
 * tests and by sim_init for lava. If the voxel's material is PHASE_LIQUID the
 * same slot also acts as an infinite FILL source - PHASE 2 re-stamps it to
 * fill=FLUID_FULL each tick, so a held liquid floods outward without draining
 * (the mass analogue of the temperature hold). Returns 0 on success, non-zero if
 * the source table is full. */
int  sim_set_source(SimState *s, uint16_t li, uint8_t hold_code);

/* Like sim_set_source, but ALSO marks the source an inexhaustible LIQUID SPRING:
 * a PHASE_LIQUID spring is re-filled to brim every tick (the mass analogue of the
 * temperature Dirichlet hold), so it floods outward without draining. Use for a
 * deliberate spring; plain held sources (e.g. lava) hold HEAT ONLY via
 * sim_set_source and do NOT flood (their fill is conserved). */
int  sim_set_spring(SimState *s, uint16_t li, uint8_t hold_code);

/* 0.5 M3: set this chunk's radial-DOWN face (SIM_NEIGH index 0..5) for the binary
 * water flow - the face whose direction is most "toward the planet centre" at this
 * chunk's position. main.c computes it per-chunk; out-of-range is ignored. Default
 * after sim_init is 3 (-Y), the flat-gravity fallback for tests / the prototype. */
void sim_set_down_face(SimState *s, int face);

/* Attach (or detach) the OPTIONAL progression event sink (ARCHITECTURE Section
 * 9). `sink` is a BORROWED ProgressSink* (a ProgressRing the caller owns); the
 * sim only ever pushes to it, never frees it. Pass NULL to detach (the default
 * after sim_init): a NULL sink means the sim emits NOTHING and is byte-identical.
 *
 * USAGE: call this AFTER sim_init (sim_init leaves progress == NULL, the safe
 * default; this setter installs the borrowed ring post-init). Idempotent; may be
 * called again to swap or clear the sink. It touches ONLY s->progress - it never
 * reads the ring, alters a voxel, or changes a tick's outcome, so the read-only
 * observer invariant holds.
 *
 * INLINE on purpose: the body is a single guarded assignment, so it lives in the
 * header (no sim.c definition is required, and nothing else in sim.c needs to
 * change to wire the field). The emit SITES inside sim_tick / try_phase_change -
 * the `if (s->progress) prog_emit(...)` pushes - are the sim.c-side work; this
 * setter is only how the caller installs the borrowed ring. */
static inline void sim_set_progress_sink(SimState *s, ProgressSink *sink)
{
    if (s != NULL)
        s->progress = sink;
}

/* Release sim resources. (SimState is caller-owned/stack-or-heap; this clears
 * the front and source tables. No allocation is owned by SimState in this
 * milestone, so this is mostly symmetry with sim_init for forward-compat.) */
void sim_shutdown(SimState *s);

/* ---- Viz helper (read by the mesher) ------------------------------------- *
 * Map an 8-bit voxel temperature code to a 0..15 glow level the mesher
 * packs into the high nibble of the face light byte. Pure integer ramp over the binding
 * codec window [SIM_GLOW_LO_CODE..SIM_GLOW_HI_CODE]; below the window -> 0, at or
 * above -> 15. Inline so the mesher pays only a couple of ALU ops per face. */
static inline uint8_t sim_temp_glow(uint8_t temp_code) {
    if (temp_code <= SIM_GLOW_LO_CODE)
        return 0;
    if (temp_code >= SIM_GLOW_HI_CODE)
        return 15;
    /* 1..14 across the open window; integer (avoids float at mesh time). */
    return (uint8_t)(1u + (uint32_t)(temp_code - SIM_GLOW_LO_CODE) * 14u
                              / (SIM_GLOW_HI_CODE - SIM_GLOW_LO_CODE));
}

/* ---- Heat-unit <-> code conversion (read by sim.c and tests) ------------- *
 * temp_to_heat decodes an 8-bit code to internal heat units (Celsius << 6);
 * heat_to_code re-quantizes heat units back to the nearest 8-bit code via the
 * binding temp_encode_c. These bridge the storage codec (voxel.h) and the
 * fixed-point hot loop. temp_to_heat uses temp_decode_c then shifts; the decode
 * is integer-valued Celsius so no fractional float enters the loop (the result
 * is an exact integer count of heat units). Inline. */
static inline int32_t temp_to_heat(uint8_t code) {
    /* temp_decode_c returns an integer number of degrees C (1 or 20 C steps),
     * so the cast is exact; scale into Q6 heat units. We MULTIPLY by HEAT_ONE_C
     * (== 1<<HEAT_FRAC_BITS) rather than left-shift: codes 0..39 decode to a
     * NEGATIVE Celsius (code 0 == -40 C), and `negative << n` is undefined
     * behaviour in C99/C11 (6.5.7p4) - it aborts every -fsanitize=undefined
     * build. The multiply is byte-identical for all 256 codes and well-defined
     * for negatives (this is the same safe idiom the latent-heat math uses). */
    return (int32_t)temp_decode_c(code) * HEAT_ONE_C;
}
static inline uint8_t heat_to_code(int32_t heat) {
    /* Re-quantize: heat units -> Celsius (a divide by 64, rounded) -> code.
     * The +/- half is round-to-nearest in heat units before the codec, so a
     * voxel sitting at e.g. 19.7 C internal commits to the 20 C code, not 19. */
    int32_t c = (heat >= 0) ? (heat + (HEAT_ONE_C / 2)) >> HEAT_FRAC_BITS
                            : -(((-heat) + (HEAT_ONE_C / 2)) >> HEAT_FRAC_BITS);
    return temp_encode_c((double)c);
}

/* ---- Test-only determinism hash (0.4 M0; VOXEL_DETERMINISM_HARNESS) -------- *
 * FNV-1a 64-bit over the AUTHORITATIVE single-machine sim state: the bound
 * chunk's voxel words plus the full-resolution heat[] and latent[] side arrays
 * (the 8-bit voxel temp code is only a storage VIEW; heat[] is the truth, so a
 * hash of the codes alone would miss sub-quantum divergence). Compiled ONLY when
 * VOXEL_DETERMINISM_HARNESS is defined (the `make testdeterminism` build), so it
 * is ABSENT from release binaries and never on the hot path (M6 verifies this).
 * Position-indexed (not active-set order) => independent of wake history. The
 * GL-free determinism harness (test_determinism.c) uses it to assert byte-level
 * reproducibility of the CA; in M5 it backs the two-peer render-fidelity test. */
#ifdef VOXEL_DETERMINISM_HARNESS
uint64_t sim_state_hash(const SimState *s);
#endif

#endif /* SIM_H */
