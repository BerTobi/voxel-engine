/* progress.c - The PROGRESSION LAYER observer: read-only interpretation of the
 * emergent physics the sim already computes (ARCHITECTURE.md Section 9).
 *
 * Pure C, no GL / no OS dependency (mirrors chunk.c / light.c / sim.c). It owns
 * NOTHING in the simulation: every function here READS only the drained event
 * VALUES and the const g_materials[] table, and WRITES only its own ProgressState.
 * Remove this module (or pass the sim a NULL ProgressSink) and the world
 * simulates BYTE-IDENTICALLY - the binding invariant of progress.h section 6.
 *
 * The three player-facing beats (progress.h section 1) map to the three things
 * this file does on every drained event:
 *   NOTICE     - first-occurrence-of-a-(kind,material) dedup -> a one-shot flash
 *                line + an ordered DiscoveryRecord (prog_observe_drain).
 *   UNDERSTAND - the empirical per-material journal, tightened from the EVENT's
 *                OBSERVED temp code, never from MaterialDef (journal_observe_*).
 *   REPRODUCE  - the capability tier, a PURE FUNCTION of the hottest sustained
 *                tier-milestone code (prog_tier_now / prog_can_work).
 *
 * The Celsius the player reads is temp_decode_c(observed_code) - a decode of the
 * code the physics actually committed, so "copper melts at ~1080 C" is the
 * codec-quantized observation (one code off the hidden 1085 C truth), exactly the
 * empirical fuzz the design wants. The hidden MaterialDef.melt_point_c is read in
 * ONE place only - prog_can_work - to GATE workability; it is never copied into
 * the journal's observed numbers.
 */
#include <string.h>     /* memset */

#include "progress.h"
#include "voxel.h"       /* temp_decode_c, temp_encode_c, the 8-bit codec */
#include "material.h"    /* MaterialDef, g_materials, material_get, MAX_MATERIALS */

/* =====================================================================
 *  Tier-band codes - the 200/500/1000/1500 C milestones encoded ONCE.
 * =====================================================================
 * progress.h section 5 pins the milestone ladder in CELSIUS for legibility, but
 * the comparisons run on 8-bit temp CODES (the currency the physics commits, and
 * the currency the TEMP_TIER event carries in observed_temp_code/tier_code). We
 * encode the four Celsius rungs to codes ONCE, lazily, via the binding codec -
 * NEVER hand-baking the code literals, so a codec change can never desync the
 * ladder. Index order is the ProgressTier ordinal minus one (WARM..FURNACE). */
static uint8_t  g_tier_code[4];   /* encoded {WARM, HOT, FORGE, FURNACE}        */
static int      g_tier_built;     /* 0 until the codes are encoded              */

static void tier_codes_build(void)
{
    if (g_tier_built)
        return;
    g_tier_code[0] = temp_encode_c((double)PROG_TIER_C_WARM);     /* 200 C  */
    g_tier_code[1] = temp_encode_c((double)PROG_TIER_C_HOT);      /* 500 C  */
    g_tier_code[2] = temp_encode_c((double)PROG_TIER_C_FORGE);    /* 1000 C */
    g_tier_code[3] = temp_encode_c((double)PROG_TIER_C_FURNACE);  /* 1500 C */
    g_tier_built = 1;
}

/* Which tier band (0..3) a TEMP_TIER event's tier_code names, or -1 if unknown.
 * Used to dedup TEMP_TIER discoveries on the TIER (a forge temperature milestone)
 * rather than the crossing material - so each of WARM/HOT/FORGE/FURNACE fires
 * exactly once across the whole world, not once per material that crossed it. */
static int tier_index_of(uint8_t code)
{
    int i;
    tier_codes_build();
    for (i = 0; i < 4; ++i)
        if (g_tier_code[i] == code)
            return i;
    return -1;
}

/* =====================================================================
 *  Small helpers (all pure reads; no sim state, no float in any hot path)
 * =====================================================================
 * Round-to-nearest decode of a temp code to whole degrees C for DISPLAY only -
 * the journal/flash lines read as an observation ("~1080 C"), not a spec. The
 * decode itself is the binding codec; we only round its (already integer-valued)
 * result for a clean print. Off the hot path (drain/dump run in the slack band). */
static int observed_celsius(uint8_t code)
{
    double c = temp_decode_c(code);
    return (c >= 0.0) ? (int)(c + 0.5) : -(int)((-c) + 0.5);
}

/* Index of the (kind,material) dedup bit (progress.h section 3): a flat bitset of
 * PROG_KIND_COUNT * MAX_MATERIALS bits. */
static unsigned dedup_bit_index(ProgressKind kind, uint8_t mat)
{
    return (unsigned)kind * (unsigned)MAX_MATERIALS + (unsigned)mat;
}

/* Test-and-set the dedup bit. Returns 1 if this was the FIRST occurrence (the bit
 * transitioned 0 -> 1 - a genuine discovery), 0 if it was already set (silent). */
static int dedup_test_and_set(ProgressState *ps, ProgressKind kind, uint8_t mat)
{
    unsigned bit  = dedup_bit_index(kind, mat);
    unsigned word = bit >> 5;
    uint32_t mask = 1u << (bit & 31u);
    if (ps->seen_kind_mat[word] & mask)
        return 0;                       /* already discovered: silent */
    ps->seen_kind_mat[word] |= mask;
    return 1;                           /* 0 -> 1: a new discovery     */
}

/* =====================================================================
 *  The empirical journal (progress.h section 4) - tightened from OBSERVED
 *  event values, NEVER from MaterialDef. Each helper widens a band or sets a
 *  coarse boolean; none ever reads g_materials.
 * =====================================================================
 * On MELT: seed the observed melt band on first sight, else widen it (lo=min,
 * hi=max). The band MIDPOINT is the player's best estimate; it tightens with
 * trials but never collapses onto a MaterialDef number. */
static void journal_observe_melt(MaterialJournal *e, uint8_t code)
{
    if (!(e->flags & PROG_MAT_MELT_OBSERVED)) {
        e->melt_obs_lo = code;
        e->melt_obs_hi = code;
        e->flags |= PROG_MAT_MELT_OBSERVED;
    } else {
        if (code < e->melt_obs_lo) e->melt_obs_lo = code;
        if (code > e->melt_obs_hi) e->melt_obs_hi = code;
    }
    /* A thing observed to melt is also a thing observed to flow/pool: a melted
     * cubic metre is liquid. POOLS is confirmed independently by FLUID_POOL when
     * wired, but MELT alone is enough to record the fact (section 4 fallback). */
    e->flags |= (PROG_MAT_SEEN | PROG_MAT_POOLS);
}

/* On FREEZE: record the observed re-solidification temperature. Mapped (by the
 * caller) onto the SOLID id it froze to, so "molten copper, cooled, became copper
 * at ~1080 C" lands on the copper journal entry. */
static void journal_observe_freeze(MaterialJournal *e, uint8_t code)
{
    e->freeze_obs_code = code;
    e->flags |= (PROG_MAT_SEEN | PROG_MAT_FREEZE_OBSERVED);
}

/* =====================================================================
 *  prog_init - empty observer state (progress.h section 8)
 * =====================================================================
 * No discoveries, empty journal, tier NONE. Idempotent, allocates nothing. */
void prog_init(ProgressState *ps, FILE *log)
{
    if (ps == NULL)
        return;
    memset(ps, 0, sizeof(*ps));   /* zero: all bits clear, all journal entries 0,
                                   * n_discoveries 0, max_tier_code 0 (tier NONE) */
    ps->log = log;
    tier_codes_build();           /* make sure the tier-band codes are ready */
}

/* =====================================================================
 *  prog_observe_drain - the per-frame consumer (progress.h section 8)
 * =====================================================================
 * Empty the ring into the observer. For each event: tighten the per-material
 * journal from the OBSERVED code, advance max_tier_code on a TEMP_TIER, and - on
 * the FIRST occurrence of a (kind,material) pair - fire the discovery flash and
 * append an ordered DiscoveryRecord. Returns the count of NEW discoveries fired.
 *
 * Pure consumer: it only POPs from the borrowed ring and writes its own state; it
 * never touches a voxel or the sim. Safe with sink == NULL (prog_ring_pop returns
 * 0 immediately -> drains nothing). */
int prog_observe_drain(ProgressState *ps, ProgressSink *sink)
{
    ProgressEvent ev;
    int new_discoveries = 0;

    if (ps == NULL)
        return 0;

    while (prog_ring_pop(sink, &ev)) {
        ProgressKind kind = (ProgressKind)ev.kind;

        /* ---- UNDERSTAND: tighten the empirical journal from OBSERVED values ---
         * The journal keys on the material the player RECOGNISES:
         *   - MELT:  the source solid id carried in ev.material (copper), so the
         *            melt fact lands on the copper entry.
         *   - FREEZE: ev.material is the SOURCE liquid (molten_copper); the fact
         *            belongs on the SOLID it became (MaterialDef.freezes_to), so
         *            re-solidification reads on the copper entry too.
         *   - TEMP_TIER / FLUID_POOL: the voxel's own id.
         * material_get is read here only to FOLLOW freezes_to (an id remap) and,
         * for TEMP_TIER, never for a temperature number - the observed code is
         * always the EVENT's, never MaterialDef.melt_point_c. */
        switch (kind) {
        case PROG_MELT:
            journal_observe_melt(&ps->journal[ev.material], ev.observed_temp_code);
            break;

        case PROG_FREEZE: {
            uint8_t solid = material_get(ev.material)->freezes_to;
            ps->journal[ev.material].flags |= PROG_MAT_SEEN; /* the molten id was seen */
            journal_observe_freeze(&ps->journal[solid], ev.observed_temp_code);
            break;
        }

        case PROG_TEMP_TIER:
            ps->journal[ev.material].flags |= PROG_MAT_SEEN;
            /* REPRODUCE: the demonstrated-thermodynamics summary. tier_code is the
             * tier-band code the voxel's temp first crossed; the hottest one ever
             * crossed is the only "level". Pure max over observed codes - no
             * MaterialDef read, no stored gate. */
            if (ev.tier_code > ps->max_tier_code)
                ps->max_tier_code = ev.tier_code;
            break;

        case PROG_FLUID_POOL:
            ps->journal[ev.material].flags |= (PROG_MAT_SEEN | PROG_MAT_POOLS);
            break;

        default:
            /* Unknown kind: count it as seen for safety, never crash. */
            ps->journal[ev.material].flags |= PROG_MAT_SEEN;
            break;
        }

        /* times_observed counts every transition witnessed for the material the
         * player was watching (ev.material - the recognisable id), saturating at
         * the uint16 ceiling so a flood never wraps the count. */
        if (ps->journal[ev.material].times_observed != 0xFFFFu)
            ++ps->journal[ev.material].times_observed;

        /* ---- NOTICE: first-occurrence dedup -> the one-shot discovery flash ---
         * MELT/FREEZE/FLUID_POOL dedup on the material; TEMP_TIER dedups on the
         * TIER BAND (a forge temperature milestone), so each of WARM/HOT/FORGE/
         * FURNACE fires exactly ONCE across the whole world rather than once per
         * material that happened to cross it - and a material's higher tiers are
         * no longer swallowed by its own first crossing. The TEMP_TIER row of the
         * dedup bitset uses slots 0..3 for the four bands. */
        uint8_t dedup_key = ev.material;
        uint8_t rec_temp  = ev.observed_temp_code;
        if (kind == PROG_TEMP_TIER) {
            int ti = tier_index_of(ev.tier_code);
            if (ti < 0) ti = 0;
            dedup_key = (uint8_t)ti;
            rec_temp  = ev.tier_code;   /* record the milestone, not the voxel's exact temp */
        }
        if (dedup_test_and_set(ps, kind, dedup_key)) {
            ++new_discoveries;

            /* Append the ordered DiscoveryRecord (drop-and-count past the cap;
             * the early discoveries are the interesting ones - section 3). */
            if (ps->n_discoveries < PROG_MAX_DISCOVERIES) {
                DiscoveryRecord *d = &ps->discoveries[ps->n_discoveries++];
                d->kind               = ev.kind;
                d->material           = ev.material;
                d->observed_temp_code = rec_temp;
                d->_pad               = 0;
                d->tick               = ev.tick;
            } else if (ps->discoveries_dropped != 0xFFFFu) {
                ++ps->discoveries_dropped;
            }

            /* Fire the factual flash. Phrased as something the player SAW, with
             * the OBSERVED (decoded) temperature tilde'd to read as an
             * observation, not a spec. Silent if no log stream (tests run quiet).*/
            if (ps->log != NULL) {
                const char *name = material_get(ev.material)->name;
                int c = observed_celsius(ev.observed_temp_code);
                switch (kind) {
                case PROG_MELT:
                    fprintf(ps->log,
                            "DISCOVERY: %s melts (observed ~%d C)\n", name, c);
                    break;
                case PROG_FREEZE: {
                    /* ev.material is the SOURCE liquid id (already "molten_*");
                     * the human-readable line names the SOLID it became, so
                     * "molten copper re-solidifies into copper" reads cleanly
                     * (not "molten molten_copper"). */
                    const char *solid =
                        material_get(material_get(ev.material)->freezes_to)->name;
                    (void)name;
                    fprintf(ps->log,
                            "DISCOVERY: molten %s re-solidifies into %s "
                            "(observed ~%d C)\n", solid, solid, c);
                    break;
                }
                case PROG_TEMP_TIER:
                    fprintf(ps->log,
                            "DISCOVERY: your forge sustained ~%d C\n",
                            observed_celsius(ev.tier_code));
                    break;
                case PROG_FLUID_POOL:
                    fprintf(ps->log,
                            "DISCOVERY: molten %s pooled as a liquid\n", name);
                    break;
                default:
                    break;
                }
            }
        }
    }

    return new_discoveries;
}

/* =====================================================================
 *  prog_tier_now - the EMERGENT capability tier (progress.h section 5)
 * =====================================================================
 * A PURE FUNCTION of ps->max_tier_code: the hottest tier-milestone code the
 * player was observed to sustain maps to a ladder ordinal. No stored gate, no
 * material-id switch - it compares the observed code against the tier-band codes
 * (encoded once from Celsius). Recomputable at any time. */
ProgressTier prog_tier_now(const ProgressState *ps)
{
    uint8_t code;

    if (ps == NULL)
        return PROG_TIER_NONE;
    tier_codes_build();

    code = ps->max_tier_code;
    if (code >= g_tier_code[3]) return PROG_TIER_FURNACE;  /* >= 1500 C */
    if (code >= g_tier_code[2]) return PROG_TIER_FORGE;    /* >= 1000 C */
    if (code >= g_tier_code[1]) return PROG_TIER_HOT;      /* >=  500 C */
    if (code >= g_tier_code[0]) return PROG_TIER_WARM;     /* >=  200 C */
    return PROG_TIER_NONE;
}

/* =====================================================================
 *  prog_can_work - "can I work iron?" answered by THERMODYNAMICS (section 5)
 * =====================================================================
 * A material is workable iff the player has DEMONSTRABLY sustained a temperature
 * at/above the material's OWN melt point. The hidden truth (MaterialDef.
 * melt_point_c) GATES the capability here - encoded to a code for the comparison
 * - but is NEVER copied into the journal. Data-driven, NO id switch: it compares
 * ps->max_tier_code against the material's encoded melt point. Returns 0 for a
 * material that never melts (melt_point_c < 0) or one the player cannot yet
 * sustain. */
int prog_can_work(const ProgressState *ps, uint8_t mat)
{
    const MaterialDef *md;
    uint8_t melt_code;

    if (ps == NULL)
        return 0;

    md = material_get(mat);
    if (md->melt_point_c < 0)             /* it burns / never melts: not workable */
        return 0;

    /* Encode the hidden melt point to a code (round-to-nearest codec) and compare
     * against the hottest sustained tier code. The melt point is the gate; the
     * journal never sees this number. */
    melt_code = temp_encode_c((double)md->melt_point_c);
    return (ps->max_tier_code >= melt_code) ? 1 : 0;
}

/* =====================================================================
 *  prog_journal_dump - the shutdown console journal (progress.h section 8)
 * =====================================================================
 * Prints, in order: the discovery list; then per-material OBSERVED facts for
 * every SEEN material; then the current capability tier and the materials now
 * workable. Reads MaterialDef ONLY for the material NAME (flavor) and to compute
 * workability - never to fill an observed number. No-op if ps->log is NULL. */
void prog_journal_dump(const ProgressState *ps)
{
    static const char *TIER_NAME[5] = {
        "none", "warm (~200 C)", "hot (~500 C)",
        "forge (~1000 C)", "furnace (~1500 C)"
    };
    FILE *log;
    int i, mat;
    ProgressTier tier;

    if (ps == NULL || ps->log == NULL)
        return;
    log = ps->log;

    /* ---- The ordered discovery list (Beat 1, in the order they happened) ---- */
    fprintf(log, "\n=== PROGRESSION JOURNAL ===\n");
    fprintf(log, "-- Discoveries (%u) --\n", (unsigned)ps->n_discoveries);
    if (ps->n_discoveries == 0)
        fprintf(log, "  (nothing observed yet)\n");
    for (i = 0; i < (int)ps->n_discoveries; ++i) {
        const DiscoveryRecord *d = &ps->discoveries[i];
        const char *name = material_get(d->material)->name;
        int c = observed_celsius(d->observed_temp_code);
        switch ((ProgressKind)d->kind) {
        case PROG_MELT:
            fprintf(log, "  [t%u] %s melts (observed ~%d C)\n",
                    (unsigned)d->tick, name, c);
            break;
        case PROG_FREEZE: {
            const char *solid =
                material_get(material_get(d->material)->freezes_to)->name;
            (void)name;
            fprintf(log, "  [t%u] molten %s re-solidifies into %s "
                         "(observed ~%d C)\n",
                    (unsigned)d->tick, solid, solid, c);
            break;
        }
        case PROG_TEMP_TIER:
            fprintf(log, "  [t%u] your forge sustained ~%d C\n",
                    (unsigned)d->tick, c);
            break;
        case PROG_FLUID_POOL:
            fprintf(log, "  [t%u] molten %s pooled as a liquid\n",
                    (unsigned)d->tick, name);
            break;
        default:
            break;
        }
    }
    if (ps->discoveries_dropped != 0)
        fprintf(log, "  (+%u more not recorded)\n",
                (unsigned)ps->discoveries_dropped);

    /* ---- Per-material empirical facts (Beat 2 - Understand) ----------------- *
     * For every SEEN material, narrate what was OBSERVED: the melt band (its
     * midpoint is the best estimate, the lo..hi the empirical fuzz), the freeze
     * temperature, and whether it pools/flows. The NAME is flavor only; every
     * number is a decode of an OBSERVED code, never MaterialDef. */
    fprintf(log, "-- Observed material facts --\n");
    for (mat = 0; mat < MAX_MATERIALS; ++mat) {
        const MaterialJournal *e = &ps->journal[mat];
        const char *name;
        int wrote_any = 0;

        if (!(e->flags & PROG_MAT_SEEN))
            continue;
        name = material_get((uint8_t)mat)->name;

        fprintf(log, "  %s:", name);
        if (e->flags & PROG_MAT_MELT_OBSERVED) {
            int lo = observed_celsius(e->melt_obs_lo);
            int hi = observed_celsius(e->melt_obs_hi);
            if (lo == hi)
                fprintf(log, " observed molten ~%d C", lo);
            else
                fprintf(log, " observed molten ~%d-%d C", lo, hi);
            wrote_any = 1;
        }
        if (e->flags & PROG_MAT_FREEZE_OBSERVED) {
            fprintf(log, "%s re-solidifies ~%d C",
                    wrote_any ? ";" : "",
                    observed_celsius(e->freeze_obs_code));
            wrote_any = 1;
        }
        if (e->flags & PROG_MAT_POOLS) {
            fprintf(log, "%s pools and flows", wrote_any ? ";" : "");
            wrote_any = 1;
        }
        if (!wrote_any)
            fprintf(log, " seen");
        fprintf(log, " (observed %ux)\n", (unsigned)e->times_observed);
    }

    /* ---- The emergent capability tier + what is now workable (Beat 3) ------- */
    tier = prog_tier_now(ps);
    fprintf(log, "-- Capability --\n");
    fprintf(log, "  current tier: %s\n", TIER_NAME[(int)tier]);
    fprintf(log, "  workable now:");
    {
        int any = 0;
        for (mat = 0; mat < MAX_MATERIALS; ++mat) {
            if (prog_can_work(ps, (uint8_t)mat)) {
                fprintf(log, " %s", material_get((uint8_t)mat)->name);
                any = 1;
            }
        }
        if (!any)
            fprintf(log, " (nothing yet)");
        fprintf(log, "\n");
    }
    fprintf(log, "===========================\n");
}

/* =====================================================================
 *  Test / diagnostic accessors (progress.h section 8) - pure reads
 * ===================================================================== */

/* Has the player discovered (kind,material) yet? (the dedup bit). */
int prog_has_discovered(const ProgressState *ps, ProgressKind kind, uint8_t mat)
{
    unsigned bit, word;
    uint32_t mask;
    if (ps == NULL)
        return 0;
    bit  = dedup_bit_index(kind, mat);
    word = bit >> 5;
    mask = 1u << (bit & 31u);
    return (ps->seen_kind_mat[word] & mask) ? 1 : 0;
}

/* The per-material journal entry (borrowed const pointer; never NULL for a valid
 * id - mat is a uint8 so it always indexes within journal[MAX_MATERIALS]). */
const MaterialJournal *prog_journal_of(const ProgressState *ps, uint8_t mat)
{
    if (ps == NULL)
        return NULL;
    return &ps->journal[mat];
}
