/* progress.h - The PROGRESSION LAYER: discovery as read-only OBSERVATION of the
 * physics the sim already computes (ARCHITECTURE.md Section 9).
 *
 * Binding source: ARCHITECTURE.md Section 9 ("Progression Layer - Discovery as
 * Observation, Not Authorship"), the binding rule restated in Section 3.5/9
 * ("the simulation stays purely physical; progression is a read-only observer;
 * remove the progression layer and the world simulates BYTE-IDENTICALLY"), the
 * MaterialDef hidden truths of Section 2.3 (material.h), and the state-transition
 * emit point of Section 3.5/3.8 (sim.c try_phase_change, and the heat commit).
 *
 * This milestone is CONSOLE-JOURNAL only: the engine has no text/UI/font yet, so
 * the player-facing artifact (the journal) is emitted as stderr/log lines now -
 * a discovery flash on the first occurrence of a kind, and a journal dump on
 * shutdown. The in-world HUD is a later milestone; nothing here precludes it
 * (the same ProgressState drives both). Verification is therefore test-based +
 * console-journal output, NOT a screenshot.
 *
 * =====================================================================
 *  1. THE PROGRESSION THESIS  (the opinionated design - Section 9 is OPEN)
 * =====================================================================
 * The doc's central claim, which this module is built to honor: EMERGENCE IS THE
 * SIMULATION'S JOB; LEGIBILITY IS PROGRESSION'S JOB. The CA already melts copper
 * when a stone cavity holds heat past 1085 C - no recipe, no unlock, just heat
 * crossing a MaterialDef threshold. What the player lacks is not the physics but
 * the NOTICING: melt iron by accident, fail to see it, fail to understand why,
 * fail to reproduce it -> the physics worked and the game failed. So this layer
 * does exactly one thing: it WATCHES the sim produce emergent transitions and
 * INTERPRETS them for the player, never touching the sim itself.
 *
 * The player-facing loop, concretely, has three beats (Section 9 "Notice /
 * Understand / Reproduce"):
 *   NOTICE     - the sim does something physical (ore pools into liquid). The
 *                observer fires a one-shot factual flash: "DISCOVERY: copper
 *                melts (observed ~1080 C)". Phrased as something the player SAW,
 *                because they did. Deduped so it fires ONCE and goes quiet.
 *   UNDERSTAND - the journal accumulates what has been OBSERVED about each
 *                material: the temperature it was first seen molten, the
 *                temperature it re-solidified, whether it pools/flows. These are
 *                recorded from the EVENT's observed values and CONVERGE toward
 *                (but are recorded independently of) the hidden MaterialDef truth.
 *   REPRODUCE  - a CAPABILITY TIER emerges: "you can work copper" is not a flag,
 *                it is the FACT that the player has demonstrably sustained a
 *                temperature at/above copper's melt point. The tier is a pure
 *                function of demonstrated thermodynamics, computed from the
 *                journal, never an authored tech-tree node.
 *
 * THE HONEST RISK (Section 9, stated plainly, not papered over): an emergent
 * progression with NO authored backbone is, by default, unfun and illegible -
 * the player melts iron, never notices, never reproduces. This module's design
 * MITIGATES that without scripting, in three load-bearing ways:
 *   (a) DEDUPED FACTUAL NOTIFICATION - the first occurrence of a kind is made
 *       loud (a discovery line) so the "aha" lands; later occurrences are silent
 *       (no achievement-spam), so the signal is never buried (Section 9 "No
 *       achievement-spam").
 *   (b) AN EMPIRICAL JOURNAL, NOT AN ACHIEVEMENT LIST - the journal records what
 *       the player LEARNED about the world ("copper turns liquid at ~1080 C"),
 *       not merely THAT something happened. Legibility is the deliverable.
 *   (c) TIERS GROUNDED IN PHYSICS THE PLAYER CONTROLS - the only "level" is the
 *       hottest temperature the player's constructions have sustained, which is
 *       a property of voxels they placed. Progress is always traceable to a
 *       physical act.
 * What this module DELIBERATELY does NOT do (Section 9 "What I Refuse To Do"):
 * no tech tree, no XP/levels/currency, no unlock() gate, no hook inside the sim
 * tick. The authored "backbone" of hints at discoverability cliffs (Section 9
 * aids #1-#3, the iron-threshold ramp) is a LATER, data-driven layer that reads
 * this same observed state; it is NOT in this contract and would still gate
 * nothing and write nothing into the sim. This header pins the OBSERVER; the
 * hints sit on top of it.
 *
 * =====================================================================
 *  6. THE READ-ONLY CONTRACT  (THE binding invariant - a test asserts it)
 * =====================================================================
 * (Stated up front because everything below is shaped by it.)
 *
 *   > The observer may READ simulation state and CONSUME an event stream. It may
 *   > NEVER write a voxel, alter a MaterialDef, or change a tick's outcome. If
 *   > you remove the entire progression layer, the world simulates BYTE-IDENTICALLY.
 *
 * How the architecture guarantees this:
 *   - The sim's voxel updates depend on NOTHING the observer does. The observer
 *     is a pure SINK: events flow sim -> observer, never observer -> sim.
 *   - Emission is OPTIONAL. The sim holds a single nullable pointer to a
 *     ProgressSink (see section 7). When it is NULL, the emit hook is a no-op
 *     (an `if (sink) ...` the branch predictor pins false): the sim does not
 *     even build an event. NULL sink => zero observation => byte-identical sim.
 *     This is also HOW the invariant is tested (section 9): run the same world
 *     WITH a sink vs WITHOUT (NULL), assert the chunk voxel words are identical
 *     at every tick.
 *   - The sink the sim writes is a bounded RING the sim only ever PUSHES to and
 *     the observer only ever DRAINS from. The push is O(1) and float-free (it
 *     copies already-computed integers: kind, material id, the 8-bit temp code,
 *     coords, tick). It allocates nothing and calls back into no code (no
 *     observer pattern in the sim - Section 1.2 forbids it on the hot path; the
 *     ring is a single-producer/single-consumer queue, the doc's sanctioned
 *     "work queue drained at a fixed point in the frame").
 *   - The observer's own state (ProgressState) is entirely separate memory. It
 *     reads the drained events and the (const) MaterialDef table; it never holds
 *     a mutable pointer into the sim.
 *
 * The ProgressSink (the ring) is OWNED by the caller (main.c), passed to the sim
 * by borrowed pointer, and drained by the observer each frame. It is NOT part of
 * SimState's identity: zeroing/serialising SimState is unchanged. This keeps the
 * sim's determinism and persistence contracts untouched.
 */
#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdint.h>
#include <stdio.h>      /* FILE* for the journal dump sink (stderr now)         */
#include "voxel.h"      /* Voxel, temp_decode_c - the 8-bit temp codec          */
#include "material.h"   /* MaterialDef, MAX_MATERIALS - the hidden truths        */

/* =====================================================================
 *  2. EVENT MODEL  (the ONLY channel sim -> observer; Section 9 event ring)
 * =====================================================================
 * The sim EMITS a ProgressEvent on emergent transitions it ALREADY computes.
 * Every kind is grounded in physics the current sim genuinely produces today:
 *   - PROG_MELT / PROG_FREEZE: try_phase_change completes a material swap
 *     (solid->liquid sets VF_LIQUID; liquid->solid clears it). The natural emit
 *     point: the two `wake_ring(s, li); return 1;` completion sites in sim.c.
 *   - PROG_TEMP_TIER: a voxel's committed temperature code first crosses a
 *     coarse milestone (200/500/1000/1500 C). Emitted from the PHASE 2 commit,
 *     where the code actually changes - these are the capability gates a furnace
 *     must reach, observed as they happen.
 *   - PROG_FLUID_POOL (optional, but the sim computes it): a molten/liquid cell
 *     SETTLES as part of a multi-cell pool - i.e. it is PHASE_LIQUID, brim-full,
 *     and went from unsettled to settled (sim_liquid_unsettled flips 1->0) with
 *     same-material liquid neighbours. "Molten metal first pooled as a fluid."
 *     This kind is DEFERRABLE: a sim that does not yet emit it loses only that
 *     discovery, never correctness. The journal's `pools` fact still records
 *     from MELT alone if PROG_FLUID_POOL is never wired (see section 4).
 *
 * The event is a small tagged POD. The observed temperature is carried as the
 * 8-bit voxel temp CODE (not Celsius) so emission is FLOAT-FREE in the hot loop
 * (the codec's float decode runs only later, in the observer, off the hot path).
 * 20 bytes (4 leading u8 + three i32 world coords + a u32 tick, naturally
 * aligned), trivially copyable, no pointers - matches the Section 9 SimEvent
 * sketch (which was ~20 B). At PROG_RING_CAP=256 the ring is ~5 KiB.
 */
typedef enum {
    PROG_MELT       = 0,  /* a voxel completed solid->liquid (VF_LIQUID set)     */
    PROG_FREEZE     = 1,  /* a voxel completed liquid->solid (VF_LIQUID cleared) */
    PROG_TEMP_TIER  = 2,  /* a voxel's temp code first crossed a tier milestone  */
    PROG_FLUID_POOL = 3,  /* a liquid first settled into a multi-cell pool       */
    PROG_KIND_COUNT = 4   /* number of kinds (sizes the dedup table)             */
} ProgressKind;

/* One emitted observation. POD, 16 bytes, no pointers - safe to memcpy into the
 * ring and to drain after the producing chunk has changed/evicted (it captures
 * VALUES, never a Chunk* or Voxel*). */
typedef struct {
    uint8_t  kind;           /* ProgressKind                                     */
    uint8_t  material;       /* material id AT THE MOMENT of the event:          *
                              *  - MELT:  the SOURCE solid id (what the player    *
                              *    was watching, e.g. copper) - captured BEFORE   *
                              *    the swap so the journal keys on the thing the  *
                              *    player recognises, not the molten_* result.    *
                              *  - FREEZE: the SOURCE liquid id (the molten_*).   *
                              *  - TEMP_TIER / FLUID_POOL: the voxel's id.        */
    uint8_t  observed_temp_code; /* the voxel's 8-bit temp code AT the event -   *
                              * the OBSERVED temperature, decoded to ~Celsius by  *
                              * the observer (NOT MaterialDef.melt_point_c). For  *
                              * MELT/FREEZE this is the plateau/threshold code the *
                              * physics actually committed - quantized to the     *
                              * binding two-segment codec, which IS the grain of  *
                              * the player's empirical knowledge (Section 9).     */
    uint8_t  tier_code;      /* TEMP_TIER only: the tier milestone code crossed   *
                              * (one of PROG_TIER_CODES); 0 for other kinds.      */
    int32_t  wx, wy, wz;     /* WORLD voxel coords (chunk origin*16 + local x/y/z)*
                              * for a future "[mark location]" / HUD ping. The    *
                              * emit hook computes these from the chunk cx/cy/cz  *
                              * and the local index - cheap integer math.         */
    uint32_t tick;           /* sim tick_index at emission (dedup/ordering/log)   */
} ProgressEvent;

/* The bounded ring the sim PUSHES and the observer DRAINS (Section 9: "Fixed
 * ring, drained every frame. Sized small on purpose: the active-voxel cap
 * already bounds how many transitions a tick can produce.").
 *
 * CAPACITY: 256 entries here, not the doc's sketch of 1024. Rationale: this
 * milestone is SINGLE-CHUNK (one 4096-voxel chunk), so a tick can complete at
 * most a few hundred transitions, and the observer drains EVERY frame (2 ticks
 * per frame at 15 Hz / 30 FPS). 256 * 16 B = 4 KiB, comfortably in the misc/UI
 * budget. The cap is a tunable; a multi-chunk milestone can raise it.
 *
 * OVERFLOW POLICY (Section 9 "Event-ring overflow", load-bearing): on a flood
 * (e.g. a lava breach melting hundreds of voxels in one tick) the ring may fill
 * before the observer drains. The policy is DROP-OLDEST: a push to a full ring
 * advances the tail (discarding the oldest unread event) so the NEWEST events
 * always survive and the structure NEVER corrupts. This is acceptable for
 * PROGRESSION because discovery is first-occurrence-of-a-kind: we only need to
 * learn "iron melts" ONCE, and a flood is hundreds of copies of the SAME melt -
 * dropping duplicates loses nothing. (Section 9 suggests dedup-on-insert as the
 * ideal; drop-oldest is the simpler, equally-safe form for a first-occurrence
 * journal and is what the test asserts. The first novel event of any kind that
 * the observer has not yet drained is the only thing a flood could evict, and
 * the active-front advances one ring/tick so a single tick cannot emit hundreds
 * of DISTINCT novel kinds - the distinct-kind count is tiny and bounded.)
 *
 * The ring is a value type, zero-initialised (head==tail==0 => empty), owned by
 * the caller. It holds NO state the sim persists or that affects determinism. */
#define PROG_RING_CAP 256u   /* power of two: mask-wrap the indices              */

typedef struct {
    ProgressEvent buf[PROG_RING_CAP];
    uint32_t      head;      /* next slot to WRITE (producer: the sim)           */
    uint32_t      tail;      /* next slot to READ  (consumer: the observer)      */
    uint32_t      dropped;   /* count of drop-oldest evictions (diagnostics)     */
} ProgressRing;

/* The optional SINK the sim holds (section 7): a borrowed pointer to a ring.
 * The sim's emit hook is `if (s->progress) prog_emit(s->progress, &ev);`. When
 * the field is NULL the sim emits NOTHING and is byte-identical. ProgressSink is
 * just ProgressRing under a name that reads as the sim's dependency. */
typedef ProgressRing ProgressSink;

/* ---- Ring primitives (inline, O(1), float-free - usable from sim.c) -------- *
 * prog_emit is THE function the sim calls. It is the producer side: push one
 * event, drop-oldest on full. No allocation, no callback, no float. Safe to call
 * with sink==NULL (no-op) so the call site can stay unconditional if preferred,
 * though the sim guards with `if (s->progress)` to skip even building the event. */
static inline void prog_emit(ProgressSink *sink, const ProgressEvent *ev)
{
    uint32_t next;
    if (sink == NULL)
        return;                                  /* OPTIONAL: no sink, no-op     */
    next = (sink->head + 1u) & (PROG_RING_CAP - 1u);
    if (next == (sink->tail & (PROG_RING_CAP - 1u))) {
        /* Full: drop the OLDEST (advance tail) so the newest event survives and
         * the ring never overruns the consumer's unread region (section 2). */
        sink->tail = (sink->tail + 1u) & (PROG_RING_CAP - 1u);
        ++sink->dropped;
    }
    sink->buf[sink->head & (PROG_RING_CAP - 1u)] = *ev;
    sink->head = next;
}

/* Pop one event into *out (consumer side, the observer). Returns 1 if an event
 * was dequeued, 0 if the ring was empty. */
static inline int prog_ring_pop(ProgressSink *sink, ProgressEvent *out)
{
    if (sink == NULL)
        return 0;
    if ((sink->head & (PROG_RING_CAP - 1u)) == (sink->tail & (PROG_RING_CAP - 1u)))
        return 0;                                /* empty                        */
    *out = sink->buf[sink->tail & (PROG_RING_CAP - 1u)];
    sink->tail = (sink->tail + 1u) & (PROG_RING_CAP - 1u);
    return 1;
}

/* Reset a ring to empty (head==tail). Does not clear buf (stale entries are
 * unreachable while empty). Diagnostics counter preserved across drains; cleared
 * here for a fresh run/test. */
static inline void prog_ring_init(ProgressSink *sink)
{
    if (sink == NULL)
        return;
    sink->head = 0u;
    sink->tail = 0u;
    sink->dropped = 0u;
}

/* =====================================================================
 *  5. TIER MODEL  (capability tiers that EMERGE; Section 9 "Tiers ... Emerge")
 * =====================================================================
 * The temperature MILESTONES that double as (a) the TEMP_TIER event thresholds
 * and (b) the capability-tier ladder. These are COARSE bands the player's forge
 * must demonstrably reach - NOT material melt points (that would re-import the
 * tech-tree we refuse). A material becomes "workable" when the player has
 * sustained a temperature at/above ITS melt point; the bands below are the
 * legible rungs the journal narrates ("your forge reached ~1000 C").
 *
 * Pinned in CELSIUS for legibility; the emit hook compares against the codec
 * encoding of each (a code, computed once - the hot loop compares codes, no
 * float). 200/500/1000/1500 C per the milestone task. */
#define PROG_TIER_C_AMBIENT   0     /* below the first band: nothing demonstrated */
#define PROG_TIER_C_WARM      200   /* sustained heat - drying, low-temp work      */
#define PROG_TIER_C_HOT       500   /* a real fire - charring, baking              */
#define PROG_TIER_C_FORGE     1000  /* primitive smelting band (copper 1085 nearby)*/
#define PROG_TIER_C_FURNACE   1500  /* iron band (1538) - the Section 9 iron cliff */

/* The capability tier as an ordinal LADDER. This is the "what can the player
 * work now" summary - a pure FUNCTION of observed state (section below), a
 * CONSEQUENCE of demonstrated thermodynamics, never a stored unlock. The ordinal
 * is derived from the hottest temperature the player has demonstrably SUSTAINED
 * (the max observed tier code) - see prog_tier_now(). */
typedef enum {
    PROG_TIER_NONE    = 0,  /* nothing demonstrated yet                          */
    PROG_TIER_WARM    = 1,  /* sustained >= 200 C                                */
    PROG_TIER_HOT     = 2,  /* sustained >= 500 C                                */
    PROG_TIER_FORGE   = 3,  /* sustained >= 1000 C (copper-class smelting)       */
    PROG_TIER_FURNACE = 4   /* sustained >= 1500 C (iron-class smelting)         */
} ProgressTier;

/* =====================================================================
 *  4. EMPIRICAL JOURNAL  (per-material OBSERVED facts; Section 9 "the Journal")
 * =====================================================================
 * The journal is a DISCOVERED SHADOW of the hidden MaterialDef table: one entry
 * per material id, initially empty, filled in BY WATCHING. The decisive rule
 * (Section 9, defended hard): the journal stores OBSERVED values, NOT the true
 * MaterialDef numbers, and they CONVERGE toward truth as the player observes
 * more. The observed temperature is the codec-quantized code the physics
 * committed at the transition - in the industrial band that is 20 C/step, and
 * THAT quantization grain IS the grain of the player's empirical knowledge: the
 * player learns "copper melted at ~1080 C", which happens to sit one code off
 * copper's true 1085 C - exactly the empirical fuzz the design wants.
 *
 * CONVERGENCE (not copy): each observation TIGHTENS a range.
 *   - melt_obs_lo / melt_obs_hi: the band of OBSERVED temperatures at which this
 *     material was seen to MELT. First MELT seeds both to the observed code;
 *     later melts widen/refine the band toward (but never read) the truth. The
 *     player's "best estimate" is the band midpoint, narrowing with trials.
 *   - freeze_obs_code: the OBSERVED temperature at which the molten form was seen
 *     to re-solidify (recorded against the SOLID id it froze to, so "molten
 *     copper, cooled, became copper at ~1080 C" lands on the copper entry).
 *   - flags: SEEN / MELT_OBSERVED / FREEZE_OBSERVED / POOLS - coarse booleans the
 *     journal narrates ("observed to flow and pool").
 *   - times_observed: how many transitions of this material the player has
 *     witnessed (the band tightens with count; also drives "observed once" vs
 *     "observed many times" phrasing).
 *
 * One entry is 12 bytes; 256 * 12 = 3 KiB, trivially serialisable alongside a
 * save (a later milestone - this milestone keeps it in memory and dumps it to
 * the console on shutdown). Values are temp CODES (decode to ~Celsius for
 * display) so the journal stays in the same currency the sim committed. */
#define PROG_MAT_SEEN            0x01u  /* an event ever named this material      */
#define PROG_MAT_MELT_OBSERVED   0x02u  /* seen to melt at least once             */
#define PROG_MAT_FREEZE_OBSERVED 0x04u  /* seen to re-solidify at least once      */
#define PROG_MAT_POOLS           0x08u  /* seen to settle as a multi-cell pool    */

typedef struct {
    uint8_t  flags;            /* PROG_MAT_* observed booleans                    */
    uint8_t  melt_obs_lo;      /* lowest temp code observed melting (0=unseen)    */
    uint8_t  melt_obs_hi;      /* highest temp code observed melting (0=unseen)   */
    uint8_t  freeze_obs_code;  /* temp code observed re-solidifying (0=unseen)    */
    uint16_t times_observed;   /* count of transitions witnessed for this material*/
    uint8_t  _pad[6];          /* pad to 12 bytes; room for boil/react facts later*/
} MaterialJournal;

/* =====================================================================
 *  3. DISCOVERY MODEL  (first-occurrence-of-a-kind; Section 9 "Beat 1 - Notice")
 * =====================================================================
 * A discovery is the FIRST time the player witnesses a (kind, material) pair -
 * the first iron-melt is an "aha"; every later iron-melt is silent (it only
 * tightens the journal range). Dedup is a (kind, material) bitset: one bit per
 * pair, PROG_KIND_COUNT * MAX_MATERIALS bits. PROG_KIND_COUNT (4) * 256 = 1024
 * bits = 128 bytes. On draining an event the observer tests-and-sets the bit; a
 * 0->1 transition IS a new discovery (fire the flash + push a discovery record);
 * a 1 (already set) is silent.
 *
 * The discovery RECORD is what the player is shown on first occurrence and what
 * the shutdown dump lists in order. It captures the event values at the moment
 * of the aha (so the line reads "DISCOVERY: copper melts (observed ~1080 C)").
 * Records are appended to a bounded log (newest discoveries past the cap are
 * dropped - by then the early discoveries are the interesting ones; the cap is
 * generous relative to the ~dozen real material transitions). */
#define PROG_MAX_DISCOVERIES 64u   /* generous vs ~dozen real transitions          */

typedef struct {
    uint8_t  kind;             /* ProgressKind that triggered the discovery       */
    uint8_t  material;         /* the (kind,material) pair's material id          */
    uint8_t  observed_temp_code;/* observed temp at the aha (decode for display)  */
    uint8_t  _pad;
    uint32_t tick;             /* sim tick the discovery happened (ordering)      */
} DiscoveryRecord;

/* =====================================================================
 *  THE OBSERVER STATE  (all progression memory; SEPARATE from SimState)
 * =====================================================================
 * Everything the observer maintains. Wholly distinct from the sim's memory: it
 * holds NO mutable pointer into the sim, only the drained event values and reads
 * of the const MaterialDef table. ~3 KiB journal + 128 B dedup + ~0.5 KiB
 * discovery log + scalars => well under 4 KiB. Caller-owned (heap or static);
 * lives in the misc/UI budget, never the sim budget. */
typedef struct {
    /* The (kind,material) first-occurrence dedup bitset (section 3). */
    uint32_t        seen_kind_mat[(PROG_KIND_COUNT * MAX_MATERIALS + 31) / 32];

    /* The empirical material journal, one entry per id (section 4). */
    MaterialJournal journal[MAX_MATERIALS];

    /* The ordered discovery log shown to the player (section 3). */
    DiscoveryRecord discoveries[PROG_MAX_DISCOVERIES];
    uint16_t        n_discoveries;     /* live entries (saturates at the cap)     */
    uint16_t        discoveries_dropped;/* count past the cap (diagnostics)       */

    /* Demonstrated-thermodynamics summary that drives the EMERGENT tier
     * (section 5). max_tier_code is the hottest TEMP_TIER milestone code the
     * player has been observed to reach; the current tier is a pure function of
     * it (prog_tier_now). Kept as the observed code (not the ProgressTier) so a
     * later, finer tier ladder is a pure recompute over the same observed state. */
    uint8_t         max_tier_code;     /* hottest tier milestone code crossed     */
    uint8_t         _pad[3];

    /* The journal-output sink for discovery flashes + the shutdown dump. stderr
     * now (console journal); a HUD ring later. NULL => silent (tests run quiet).*/
    FILE           *log;
} ProgressState;

/* =====================================================================
 *  8. PLAYER-FACING OUTPUT  +  THE OBSERVER API  (Section 9 "How Discovery Feels")
 * =====================================================================
 * The observer is driven by the caller (main.c) at exactly two points, both in
 * the 4.33 ms slack band, never the sim budget:
 *   - per frame (after sim ticks): prog_observe_drain() empties the ring into
 *     the ProgressState, firing a discovery flash line on each new (kind,
 *     material) and tightening the journal from every event.
 *   - on shutdown: prog_journal_dump() prints the discovery list + the empirical
 *     facts + the current capability tier.
 */

/* Initialise the observer state to empty (no discoveries, empty journal, tier
 * NONE). `log` is the journal output stream (stderr for the console journal; may
 * be NULL to run silent, which tests use). Idempotent; allocates nothing. */
void prog_init(ProgressState *ps, FILE *log);

/* Drain the ring into the observer (the per-frame consumer; Section 9 slack-band
 * drain). For each event: tighten the per-material journal from the OBSERVED
 * values, update max_tier_code on TEMP_TIER, and - on the FIRST occurrence of a
 * (kind,material) pair - fire the discovery flash to ps->log and append a
 * DiscoveryRecord. Returns the number of NEW discoveries fired this drain (0 if
 * nothing novel). Pure consumer: never touches the sim. Safe with sink==NULL
 * (drains nothing). */
int prog_observe_drain(ProgressState *ps, ProgressSink *sink);

/* The EMERGENT capability tier as a PURE FUNCTION of observed state (section 5):
 * derive the ladder ordinal from ps->max_tier_code alone. This is a CONSEQUENCE
 * of demonstrated physics, recomputable at any time, never a stored gate. No
 * material-id switch: it compares the observed code against the tier-band codes.*/
ProgressTier prog_tier_now(const ProgressState *ps);

/* Is `mat` WORKABLE given what the player has demonstrated? A material is
 * workable iff the player has sustained a temperature at/above the material's
 * OWN melt point (read from MaterialDef - the hidden truth gates capability, but
 * is NEVER copied into the journal). This is the data-driven, no-id-switch
 * capability query: it compares ps->max_tier_code against material_get(mat)->
 * melt_point_c encoded to a code. Returns 0 for a material that never melts
 * (melt_point_c < 0) or that the player cannot yet sustain. This is how "can I
 * work iron?" is answered by thermodynamics, not a flag. */
int prog_can_work(const ProgressState *ps, uint8_t mat);

/* Dump the full journal to ps->log (the shutdown console journal, Section 9
 * "Beat 2 - Understand"): the ordered discovery list, then per-material observed
 * facts (melt range, freeze temp, pools?) for every SEEN material, then the
 * current capability tier and the set of materials now workable. No-op if
 * ps->log is NULL. Reads MaterialDef ONLY for the material NAME (flavor) and to
 * compute workability - never to fill the observed numbers. */
void prog_journal_dump(const ProgressState *ps);

/* ---- Test/diagnostic accessors (so test_progress.c asserts without poking the
 * struct internals by hand; all pure reads) ------------------------------- */

/* Has the player discovered (kind,material) yet? (the dedup bit). */
int prog_has_discovered(const ProgressState *ps, ProgressKind kind, uint8_t mat);

/* The per-material journal entry (borrowed const pointer; never NULL for a valid
 * id). Tests assert melt_obs_lo/hi are the OBSERVED codes and do NOT equal a
 * blind copy of MaterialDef.melt_point_c's code. */
const MaterialJournal *prog_journal_of(const ProgressState *ps, uint8_t mat);

#endif /* PROGRESS_H */
