/* world.h - The WorldStore: the resident-chunk registry, the slab pool that
 * backs it, and the player-centred streaming window. THIS is the contract that
 * replaces main.c's static `Chunk *chunks[CHUNK_COUNT]` 4x1x4 grid with a
 * bounded, moving window of chunks that load on the leading edge and evict on
 * the trailing edge as the player roams.
 *
 * Binding source: ARCHITECTURE.md Section 1.5 (WorldStore ownership), Section
 * 2.5 (the open-addressing chunk hash + 21-bit-per-axis packed key + the
 * cached-neigh inner-loop rule), and Section 7 (the loaded chunk window,
 * distance-based eviction, the fixed-size slab pool, "no per-chunk malloc on
 * the hot path"). Disk persistence of MODIFIED chunks is Section 8 and is OUT
 * OF SCOPE for this milestone: an evicted chunk simply returns its slab to the
 * pool and is REGENERATED FROM SEED on re-entry (worldgen is deterministic, so
 * the regenerated chunk is byte-identical to the original). The corollary,
 * noted honestly: a player-MODIFIED chunk that scrolls out of the window LOSES
 * its edits this milestone. That is acceptable here and is fixed by Section 8.
 *
 * WHAT THIS OWNS, AND WHAT IT DOES NOT
 * ------------------------------------
 * The WorldStore OWNS:
 *   - the Chunk slab pool (a single contiguous pre-allocated array of Chunk
 *     slots + a free-list stack of indices). No malloc/free per chunk; ever.
 *   - the residency hash (ChunkMap): the authoritative map (cx,cy,cz)->Chunk*.
 *   - the resident-chunk iteration list (a dense array of live slab indices,
 *     so the frame loop can walk residents without scanning the sparse hash).
 *   - the neigh[6] wiring of every resident chunk (fixed up on insert/evict).
 *   - the render-slot assignment (resident Chunk -> render slot 0..MAX-1).
 *   - the per-frame streaming work queues (pending generate / pending remesh)
 *     and their budget counters.
 *
 * The WorldStore does NOT own (it CALLS OUT to them, by direct call, matching
 * the Section 1.6 mechanism table):
 *   - worldgen (worldgen.h) - fills a freshly-popped slab's voxels from seed.
 *   - the mesher (mesher.h) + light (light.h) - turn voxels into a MeshBuffer.
 *   - the renderer (render.h) - uploads a MeshBuffer to a render slot's VBO.
 *   - the sim (sim.h) - single-chunk; binds to the demo HOME chunk when it is
 *     resident and detaches when it is not (see SIM/DEMO FIT below).
 * The WorldStore takes function-pointer / explicit callbacks for gen+mesh+
 * upload+free-slot so it stays GL-free and unit-testable (test_world.c builds
 * the store with NULL render callbacks - see "TESTABILITY" below).
 *
 * SINGLE-THREADED. A normal heap structure; no locks, no atomics. One instance
 * lives on main()'s stack (or heap - it is large; see the byte math).
 */
#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include "chunk.h"
#include "mesher.h"
#include "render.h"   /* MAX_RENDER_CHUNKS only; render.h is GL-free (it includes
                       * just mesher.h), so world.h stays compilable headless. */

/* ======================================================================== *
 *  1. PACKED CHUNK KEY  (ARCHITECTURE 2.5 - 21 bits per signed axis)        *
 * ======================================================================== *
 * Pack signed chunk coords into 64 bits, 21 bits/axis (two's-complement
 * masked). 21 bits >> the 128x128x16 chunk grid and bakes NO world bound into
 * addressing (Section 2.5 binding: "world bounds are never baked in"). Axis
 * order matches the doc: cx in [0..20], cz in [21..41], cy in [42..62].
 *
 * key==0 is RESERVED as the empty-slot sentinel. Chunk (0,0,0) packs to 0, so
 * a live (0,0,0) entry is disambiguated from "empty" by the slot's ptr (an
 * occupied slot has ptr != NULL); chunkmap_get tests ptr before key. */
static inline uint64_t chunk_key(int32_t cx, int32_t cy, int32_t cz)
{
    return  ((uint64_t)((uint32_t)cx & 0x1FFFFFu))
         |  ((uint64_t)((uint32_t)cz & 0x1FFFFFu) << 21)
         |  ((uint64_t)((uint32_t)cy & 0x1FFFFFu) << 42);
}

/* Integer finalizer mix (Section 2.5): good avalanche, no division, cheap on
 * the Pentium M. Probe positions are masked by (cap-1); cap is power-of-two. */
static inline uint32_t key_hash(uint64_t k)
{
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33; return (uint32_t)k;
}

/* ======================================================================== *
 *  2. RESIDENCY HASH  (ARCHITECTURE 2.5 - open-addressing, linear probing)  *
 * ======================================================================== *
 * The authoritative residency map and the COLD-LOOKUP path only. The binding
 * inner-loop rule (Section 1.6 / 2.5) stands: hot neighbour traversal goes
 * through each chunk's cached neigh[6] pointers; the hash is hit ONLY on
 * insert / evict / a genuine window-edge cold lookup, never per voxel.
 *
 * A slot is EMPTY iff ptr == NULL && key == 0. There are no tombstones: the
 * WorldStore never deletes from the table with a probe-chain hole. Eviction
 * uses BACKWARD-SHIFT deletion (Robin-Hood-style removal for linear probing):
 * on evict, the trailing run is shifted back to close the gap, so the table
 * stays tombstone-free and probe chains stay short across an unbounded number
 * of insert/evict cycles (the player walking forever). This is what keeps the
 * "no unbounded growth as the player roams" guarantee at the hash level. */
typedef struct {
    uint64_t key;   /* packed chunk_key(); 0 + ptr==NULL == empty slot       */
    Chunk   *ptr;   /* resident chunk, or NULL for an empty slot             */
} ChunkSlot;

/* Hash capacity. Power-of-two, sized to keep load factor < 0.7 for the window
 * (Section 2.5). For this milestone's window (see WORLD_WINDOW_CHUNKS below)
 * 4096 slots hold the window comfortably under 0.7. The doc's shipping 16384-
 * slot table is for the full r=12 / 10,000-chunk window; this milestone runs a
 * smaller window (it must stay <= MAX_RENDER_CHUNKS render slots), so the hash
 * is sized to match. Bump WORLD_HASH_CAP (still a power of two, still
 * >= WORLD_WINDOW_CHUNKS / 0.7) when the window grows toward the shipping size. */
#define WORLD_HASH_CAP   131072u /* holds the radius-32 x band-13 ceiling window (54925)
                                  * at load 0.42; power-of-two (probe mask). 128K*16 B =
                                  * 2 MiB fixed - cheap, so the hash never needs sizing */

/* ======================================================================== *
 *  3. THE LOADED WINDOW + SLAB POOL geometry                                *
 * ======================================================================== *
 * WINDOW POLICY (Section 7 "The Loaded Chunk Window", scaled for this
 * milestone): the resident set is every chunk whose (cx,cz) lies within
 * CHEBYSHEV radius WORLD_RADIUS of the player's chunk, across a FIXED VERTICAL
 * BAND [WORLD_BAND_Y0 .. WORLD_BAND_Y1] that covers the procedural surface.
 *
 * Why a band, not the full 16-layer column: the shipping doc keeps all 256 m
 * (cy 0..15) resident because the player digs/builds vertically. THIS milestone
 * has no digging yet and a near-surface heightmap, so loading the full column
 * would be ~16x the chunks for no visible terrain. A 2-layer band (cy 0..1,
 * 0..32 m) brackets the heightmap's whole [WG_HEIGHT_MIN..WG_HEIGHT_MAX] range
 * (see worldgen.h) so every surface voxel is resident. The vertical-band
 * extents are pinned here so the window count is a compile-time constant. When
 * digging/streaming-down arrives, widen the band to the full column per the doc.
 *
 *   WORLD_RADIUS = 6  -> (2*6+1) = 13 chunks per horizontal axis
 *   horizontal   = 13 x 13 = 169 chunks
 *   vertical band= 2 layers (cy 0..1)
 *   WINDOW       = 169 * 2 = 338 resident chunks
 *
 * COSTS:
 *   resident voxels : 338 * 16 KiB = 5.28 MiB   (Section 7 voxel line; the
 *                     shipping 156 MiB window is the r=12 full-column target)
 *   render slots    : 338 <= MAX_RENDER_CHUNKS (4096)   -> fits, big headroom
 *   hash load factor: 338 / 4096 = 0.082                -> far under 0.7
 *
 * As the player crosses ONE chunk boundary, a 13x2 = 26-chunk curtain enters
 * on the leading edge and an equal curtain leaves the trailing edge. The
 * per-frame budget (Section 6) spreads that work over a few frames; behind the
 * G70's fog the late-arriving chunks are invisible (Section 7 streaming lead). */
/* 0.5: view distance is now RUNTIME-adjustable (the player asked for it). WORLD_RADIUS
 * is the compile-time MAXIMUM (it sizes the slab/render/hash pools, so it cannot grow
 * at runtime); the active window uses ws->view_radius in [2, WORLD_RADIUS], defaulting
 * to WORLD_VIEW_RADIUS_DEFAULT. Raising the MAX grows the reserved pools (more RAM) +
 * must stay under MAX_RENDER_CHUNKS: at 10, (2*10+1)^2*9 = 3969 chunks + a churn
 * curtain (4158) < 8192 slots, and the hash holds it at load 0.48. Slab pool ~68 MiB
 * reserved (fine on the 1 GiB XP box). The DEFAULT keeps the grain study's budgeted
 * radius-6 window (1521 chunks) unless the player turns view distance up; the FPS
 * counter lets them find the playable radius on the real hardware. */
#define WORLD_RADIUS     32                      /* MAX Chebyshev radius (256 m)  */
#define WORLD_VIEW_RADIUS_DEFAULT 6              /* initial active radius (light) */
#define WORLD_VIEW_RADIUS_MAX_DEFAULT 10         /* INITIAL session pool size (no env): the
                                                  * proven ~68 MiB, so a plain launch never
                                                  * regresses. NOT a cap - world_set_view_radius
                                                  * GROWS the pool on demand (Up key) up to
                                                  * WORLD_RADIUS / 256 m as RAM allows; an
                                                  * explicit VOXEL_VIEW_RADIUS just pre-sizes it. */
#define WORLD_DIAM       (2 * WORLD_RADIUS + 1)  /* 65 at the ceiling             */
/* Window-chunk count for an arbitrary Chebyshev radius r (the player-following band
 * is WORLD_BAND_H layers tall). Used to size the runtime slab pool to the chosen view
 * distance + by the tests; WORLD_WINDOW_CHUNKS below is this at the compile ceiling. */
#define WORLD_WINDOW_AT(r) (((2*(r))+1) * ((2*(r))+1) * WORLD_BAND_H)

/* 0.5 M2: the resident vertical band now FOLLOWS THE PLAYER. At R=64 the whole
 * ball fit a fixed cy 0..8 band; at R=512 (256 m) the planet is 64 chunks across
 * and the surface sits at high Y, so a fixed band would leave the player in
 * unloaded space. The band is a 9-layer window [center_cy-HALF .. center_cy+HALF]
 * re-anchored on the player's chunk-Y each frame (alongside the horizontal cx/cz
 * Chebyshev radius), so the resident set is a player-centred box that always
 * brackets the local surface. Count is unchanged (13x13x9 = 1521), so the slab
 * pool / render-slot budgets the grain study fixed stay put. The band is in cy
 * (world-Y) regardless of the local radial "up"; being player-centred it still
 * covers the surface anywhere, just anisotropically (a full isotropic 3-D box is
 * a 0.6 refinement if the player roams far from the spawn pole). */
/* 0.5 (256 m view distance): the band is now 6 (±96 vox) not 4. WHY: the window is a
 * player-centred CYLINDER (horizontal Chebyshev radius x this vertical band) but the
 * world is a SPHERE, so across a large view radius the surface CURVES DOWN below a
 * short band and the far ring drops out of residency - showing as holes/floating
 * chunks INSIDE the view distance. Band 6 keeps the surface resident out to ~radius 16
 * (~128 m), about the planet's visible horizon, so the visible range has no gaps;
 * beyond the horizon the surface curving away (sky) is correct, not a bug. Cost: the
 * window is ~44% taller (more underground layers near the pole), paid at every radius.
 * A radius-coupled / asymmetric band (cheap near, tall far) is the efficient 0.6
 * refinement; 6 is the simple fix that covers the useful range. */
#define WORLD_BAND_HALF  6                       /* layers above/below center_cy */
#define WORLD_BAND_H     (2 * WORLD_BAND_HALF + 1)            /* 13 layers     */
#define WORLD_WINDOW_CHUNKS (WORLD_DIAM * WORLD_DIAM * WORLD_BAND_H) /* 1521    */

/* SLAB POOL (Section 7 "Fixed-Size Slab Pool, No Per-Chunk malloc"). A single
 * pre-allocated contiguous array of Chunk slots + a free-list STACK of indices.
 * Bounded at the window size plus streaming-churn SLACK (the leading curtain
 * can be popped before the trailing curtain has finished returning its slabs
 * within one budgeted frame, so the pool must briefly hold window + one curtain
 * worth). Slack = 2 full leading curtains (2 * WORLD_DIAM * WORLD_BAND_H = 52),
 * rounded up. The pool NEVER grows: world_insert pops a free index, world_evict
 * pushes it back; a full pool is a hard error (the window is sized so it cannot
 * happen under the distance policy, asserted in tests).
 *
 *   pool slots : WORLD_WINDOW_CHUNKS + WORLD_POOL_SLACK = 338 + 64 = 402
 *   pool bytes : 402 * sizeof(Chunk) ~= 402 * 16 KiB ~= 6.3 MiB (one block)
 *
 * The pool is reserved ONCE at world_init (Section 7 "reserve big and early"):
 * one calloc of WORLD_POOL_SLOTS * sizeof(Chunk). On the XP target this single
 * contiguous block is grabbed before the heap fragments. */
#define WORLD_POOL_SLACK  (2u * WORLD_DIAM * WORLD_BAND_H)  /* 2 leading curtains
                                 * (=306 at the radius-8 max); scales with the max so a
                                 * curtain-plus always fits during a budgeted move */
#define WORLD_POOL_SLOTS  (WORLD_WINDOW_CHUNKS + WORLD_POOL_SLACK)  /* 1521+256 = 1777 */

/* SLAB SUB-POOL (0.5 M1 sparse-air storage). A Chunk RECORD (~96 B: coords,
 * neigh[6], flags, the voxels pointer + uniform_word) is always resident in the
 * pool above, but its 16 KiB VOXEL BLOCK is now drawn lazily from this separate
 * fixed pool only when the chunk is non-uniform (has solid/water content).
 * UNIFORM-AIR chunks hold voxels==NULL + a uniform_word and borrow NO slab, so they
 * cost only the ~96 B record. Above/at the surface, where much of the window is sky,
 * this means most slabs go UNTOUCHED (a working-set win on demand-paged systems) and
 * the air hemisphere costs zero CA + zero mesh. The chunk-RECORD pool also shrinks
 * from 27.8 MiB (inline voxels) to ~170 KiB. See SIZING below for why the reserved
 * slab pool itself must still cover the full window (the underground worst case).
 *
 * SIZING: the WORST reachable window is one fully BELOW the surface - a player who
 * digs/flies deep sits in solid rock, so EVERY chunk in the window needs a slab (the
 * surface 72%-air saving does NOT hold underground). So the slab pool must cover the
 * ACTIVE window. 0.5 (256 m view distance): the active window is now RUNTIME (the
 * player's chosen view distance, up to the radius-32 ceiling), and the radius-32
 * window's slab pool is ~640 MiB - far too much to reserve always on the 1 GiB XP box.
 * So the slab VOXEL pool (ws->slabs) is sized AT world_init to the session's view-
 * distance CEILING (ws->view_radius_max), NOT this compile ceiling: a 96 m session
 * reserves ~95 MiB, a 256 m session ~640 MiB (which only a big-RAM machine can give -
 * world_init fails gracefully otherwise). world_set_view_radius then clamps the active
 * radius to [2, view_radius_max], so the active window always fits ws->slab_slots and
 * world_realize can never fail. The slab_free[] INDEX stack stays sized at the compile
 * ceiling (WORLD_SLAB_SLOTS, ~150 KiB - cheap); only ws->slab_slots entries are live. */
#define WORLD_SLAB_SLOTS  WORLD_POOL_SLOTS   /* index-array size (ceiling); ws->slab_slots
                                              * is the RUNTIME live count (<= this) */

/* Every resident chunk owns a render slot, and a window move briefly holds the window
 * PLUS one leading curtain (DIAM*BAND_H) before the trailing curtain evicts. Render
 * slots must cover that peak, or a slot allocation fails mid-move. Guards a future
 * WORLD_RADIUS bump against silently exceeding MAX_RENDER_CHUNKS. */
_Static_assert(MAX_RENDER_CHUNKS >= WORLD_WINDOW_CHUNKS + WORLD_DIAM * WORLD_BAND_H,
               "render-slot pool must cover the window + a churn curtain (raise MAX_RENDER_CHUNKS)");

/* ======================================================================== *
 *  4. PER-FRAME STREAMING BUDGET  (ARCHITECTURE Section 6 / Section 7)      *
 * ======================================================================== *
 * A big window move (the player crossing a boundary) queues up to a 26-chunk
 * curtain of generates + a ring of remeshes. Draining all of it in one frame
 * would hitch the 30 FPS loop, so the work is CAPPED PER FRAME and the
 * remainder carried to later frames (Section 6 "time-bounded work-queue
 * drain", Section 7 "hard cap of N chunk-loads per frame"). The leading edge
 * is 192 m out behind fog (Section 7 streaming lead) so a chunk still
 * generating for a few frames is invisible.
 *
 *   WORLD_GEN_BUDGET    : max chunks GENERATED (popped + worldgen + linked) /frame
 *   WORLD_REMESH_BUDGET : max chunks MESHED + uploaded            / frame
 * Gen and remesh are separate budgets because a freshly-generated chunk and a
 * boundary chunk whose neighbour-set changed both need meshing, and a generate
 * also implies one mesh of itself. Sized so a full 26-chunk curtain drains in
 * ~4 frames (well inside the fog lead) without spending the whole frame slack. */
#define WORLD_GEN_BUDGET     8
#define WORLD_REMESH_BUDGET  16

/* Pending-work queue capacities. Bounded: at most the whole window could be
 * queued for (re)mesh after a teleport, and a curtain-plus for gen. Sized to
 * the window so the queues never overflow under the distance policy. */
#define WORLD_GENQ_CAP      WORLD_POOL_SLOTS
#define WORLD_REMESHQ_CAP   WORLD_POOL_SLOTS

/* ======================================================================== *
 *  5. RENDER-SLOT MAP                                                        *
 * ======================================================================== *
 * Each resident chunk owns one render slot in [0, MAX_RENDER_CHUNKS) for the
 * lifetime of its residency. A free-slot STACK hands them out on insert and
 * reclaims them on evict (render_upload_chunk(slot, empty) marks a reclaimed
 * slot drawable-as-nothing until reused). MAX_RENDER_CHUNKS (4096, render.h) >>
 * WORLD_WINDOW_CHUNKS (338), so a slot is always available. The frame loop
 * draws every LIVE resident slot (frustum culling is deferred - Section 5.2 -
 * the window is small and the G70 eats 338 draw calls). */

/* ======================================================================== *
 *  6. STREAMING RENDER CALLBACKS  (keeps world.c GL-free + testable)        *
 * ======================================================================== *
 * The WorldStore mesh/upload steps call OUT to the renderer (Section 1.6:
 * "Mesher -> Renderer = direct call"). To keep world.c free of any GL include
 * and unit-testable headless, the renderer entry points are passed as function
 * pointers at world_init. test_world.c passes NULL callbacks (it asserts
 * residency / neigh / pool invariants, never pixels), so the store skips the
 * upload step and runs pure-C.
 *
 *   gen_cb      : fill a freshly-popped slab from seed. Defaults to a worldgen.h
 *                 binding (worldgen_fill_chunk) when NULL is NOT acceptable - so
 *                 this one is REQUIRED (the store cannot invent terrain).
 *   mesh_upload_cb : light + mesh + upload one chunk into its render slot. NULL
 *                 in headless tests -> the store still meshes-CPU-side if a
 *                 mesh buffer is provided, or skips entirely (see WorldCallbacks).
 *   slot_free_cb : mark a render slot empty on evict. NULL -> no-op (tests).
 */
typedef struct WorldStore WorldStore;   /* opaque-ish; full def below          */

/* Forward declaration only (persist.h Section 5): the WorldStore holds ONE
 * optional PersistStore* and reaches it solely through pointer-to-incomplete, so
 * world.h need NOT pull in persist.h (and thus does not push it onto mesher.c /
 * render.c / main.c / the tests, which include world.h but never touch the save
 * format). world.c includes persist.h itself to call the persist_* functions.
 * A NULL store == persistence disabled == today's pure-regen behaviour, so
 * test_world.c (which never sets a store) is byte-for-byte unaffected. */
struct PersistStore;
/* Guard the typedef so a TU that includes BOTH world.h and persist.h (world.c)
 * does not redefine it - a duplicate typedef is a constraint violation under C99
 * -Wpedantic (C11 would allow the identical repeat). Whichever header is seen
 * first defines the name; the other skips it. The struct tag forward-declaration
 * above is harmless to repeat. */
#ifndef PERSIST_STORE_TYPEDEF
#define PERSIST_STORE_TYPEDEF
typedef struct PersistStore PersistStore;
#endif

/* Generate the voxels of chunk (cx,cy,cz) into `c` deterministically from the
 * world seed. Implemented by worldgen.c (worldgen_fill_chunk wrapped with the
 * store's seed). REQUIRED (non-NULL). */
typedef void (*WorldGenFn)(Chunk *c, int cx, int cy, int cz, uint64_t seed,
                           void *user);

/* Light + mesh + upload chunk `c` into render `slot`. Called for a freshly
 * generated chunk and for any boundary chunk whose neigh-set changed (re-mesh
 * to fix seams). MAY be NULL (headless tests). */
typedef void (*WorldMeshUploadFn)(Chunk *c, int slot, void *user);

/* Release render `slot` (mark drawable-as-nothing). Called on evict. MAY be
 * NULL (headless tests). */
typedef void (*WorldSlotFreeFn)(int slot, void *user);

/* 0.5 M1 sparse-air: return 1 iff chunk (cx,cy,cz) would generate as WHOLLY AIR,
 * so world_insert can skip the slab + the fill and set it uniform-air instead.
 * MAY be NULL -> the store never sparsifies (always realizes + gens, the pre-0.5
 * dense behaviour, so a store without it is byte-for-byte unchanged). The real
 * engine binds worldgen_chunk_all_air; the tests leave it NULL. */
typedef int (*WorldIsAirFn)(int cx, int cy, int cz, uint64_t seed, void *user);

typedef struct {
    WorldGenFn        gen;          /* REQUIRED                                */
    WorldMeshUploadFn mesh_upload;  /* may be NULL (headless)                  */
    WorldSlotFreeFn   slot_free;    /* may be NULL (headless)                  */
    WorldIsAirFn      is_air;       /* may be NULL (no sparse-air -> dense)    */
    void             *user;         /* opaque ctx threaded into every callback */
} WorldCallbacks;

/* ======================================================================== *
 *  7. THE WORLDSTORE STRUCT                                                  *
 * ======================================================================== *
 * One instance. Large (the slab pool dominates: ~6.3 MiB), so allocate it on
 * the heap in main(), not the stack. All arrays are FIXED-SIZE: nothing grows
 * as the player roams - the binding "bounded resident set" invariant. */
struct WorldStore {
    /* ---- residency hash (Section 2.5) ---- */
    ChunkSlot   table[WORLD_HASH_CAP];   /* open-addressing, linear probing     */
    uint32_t    table_count;             /* live residents                      */

    /* ---- chunk-record pool (Section 7) ---- */
    Chunk      *pool;                    /* one contiguous calloc, WORLD_POOL_SLOTS */
    uint32_t    free_idx[WORLD_POOL_SLOTS]; /* free-list stack of pool indices  */
    uint32_t    free_top;                /* count of free slots (stack height)  */

    /* ---- voxel-block slab sub-pool (0.5 M1 sparse-air) ---- *
     * One contiguous calloc of WORLD_SLAB_SLOTS * CHUNK_VOXELS voxels + a
     * free-list stack of slab indices. world_realize pops a block for a
     * non-uniform chunk (c->slab_idx, c->voxels = &slabs[idx*CHUNK_VOXELS]);
     * world_evict / world_set_uniform push it back. slab_inuse_peak tracks the
     * high-water mark so test_sparse can assert the pool sizing holds. */
    Voxel      *slabs;                   /* ws->slab_slots * CHUNK_VOXELS words (runtime) */
    uint32_t    slab_slots;              /* RUNTIME slab count, sized to view_radius_max  */
    uint32_t    slab_free[WORLD_SLAB_SLOTS]; /* free-list stack (ceiling-sized array)    */
    uint32_t    slab_free_top;
    uint32_t    slab_inuse_peak;         /* max slabs ever simultaneously realized */

    /* ---- resident iteration list ---- *
     * Dense array of pool indices of LIVE residents, so the frame loop and the
     * window-update walk residents without scanning the sparse hash. Each
     * resident's position here is mirrored in Chunk so eviction is O(1)
     * swap-remove (no scan). pool[resident[i]] is the i-th resident chunk. */
    uint32_t    resident[WORLD_POOL_SLOTS];
    uint32_t    resident_count;

    /* ---- render-slot free stack ---- */
    int         slot_free[MAX_RENDER_CHUNKS]; /* free render-slot indices       */
    uint32_t    slot_free_top;

    /* ---- per-frame streaming queues (Section 6) ---- *
     * gen queue holds keys (coords) to GENERATE; remesh queue holds pool
     * indices to RE-MESH (boundary chunks whose neigh-set changed, deduped by
     * the chunk's CHUNK_DIRTY_MESH flag so a chunk is queued at most once). */
    int32_t     genq_cx[WORLD_GENQ_CAP];
    int32_t     genq_cy[WORLD_GENQ_CAP];
    int32_t     genq_cz[WORLD_GENQ_CAP];
    uint32_t    genq_head, genq_tail;    /* ring indices                        */
    uint32_t    remeshq[WORLD_REMESHQ_CAP]; /* pool indices                     */
    uint32_t    remeshq_head, remeshq_tail;

    /* ---- streaming origin ---- */
    int         have_center;             /* 0 until the first world_stream_update */
    int         center_cx, center_cz;    /* player chunk the current window centres on */
    int         center_cy;               /* 0.5 M2: player chunk-Y the band follows    */
    int         view_radius;             /* 0.5: active Chebyshev radius [2..view_radius_max] */
    int         view_radius_max;         /* session CEILING (slab pool sized for this)       */

    uint64_t    seed;                    /* world is a function of this (Section 7) */
    WorldCallbacks cb;                   /* gen / mesh-upload / slot-free        */

    /* ---- optional persistence (persist.h Section 5, ARCHITECTURE Section 8) ---- *
     * The save handle the store calls at the LOAD (world_insert -> persist_load_
     * chunk), EVICT (world_evict's if(CHUNK_MODIFIED) -> persist_save_chunk) and
     * SHUTDOWN (world_shutdown -> persist_flush/persist_close) hooks. NULL == no
     * save dir == edits are ephemeral (pure regen from seed), which is exactly
     * the M7 behaviour and what the headless tests run with. Injected via
     * world_set_persist AFTER world_init (which zeroes this to NULL), so the
     * world_init signature - and every existing caller - is unchanged. */
    PersistStore *store;

    /* ---- per-chunk side channel, indexed by pool slot ---- *
     * Fields the store needs ON each Chunk but that chunk.h does not declare;
     * kept in PARALLEL arrays (indexed by pool slot = c - ws->pool) rather than
     * widening chunk.h. render_slot is the render slot the chunk owns (-1 ==
     * none); resident_at is its index in resident[] for O(1) swap-remove on
     * evict. A slot is "live" iff pool[i] is currently inserted (tracked via the
     * resident list); these arrays are only meaningful for live slots. */
    int         render_slot[WORLD_POOL_SLOTS];
    uint32_t    resident_at[WORLD_POOL_SLOTS];
};

/* Side-channel fields the WorldStore needs ON each Chunk but that chunk.h does
 * not declare (chunk.h's Chunk is the mesher/sim-facing struct). Rather than
 * widen chunk.h, the store keeps these in PARALLEL arrays indexed by pool slot
 * via the macros below. (Pool index of a Chunk* = (c - ws->pool).) This keeps
 * chunk.h untouched and the store's per-chunk bookkeeping local.
 *   render_slot : the render slot this chunk owns (-1 == none)
 *   resident_at : its index in ws->resident[] (for O(1) swap-remove on evict)
 * Stored in two more fixed arrays on the struct: */
/* (declared as the final two arrays so the big pool/table dominate the layout) */

/* ======================================================================== *
 *  8. LIFECYCLE                                                             *
 * ======================================================================== */
/* Allocate the pools, zero the hash, fill the free stacks, store the seed and
 * callbacks. cb.gen MUST be non-NULL. `view_radius_max` is the session's view-distance
 * CEILING (chunks, clamped to [2, WORLD_RADIUS]): the VOXEL slab pool is sized to it
 * (so a 256 m / radius-32 session reserves ~640 MiB while a default session reserves a
 * fraction), and the active view radius is later clamped to it. Returns 0 on success,
 * non-zero on allocation failure (e.g. view_radius_max too large for available RAM -
 * the caller should report it and suggest a smaller view distance). After this the
 * store is empty (no residents); the first world_stream_update fills the window. */
int  world_init(WorldStore *ws, uint64_t seed, const WorldCallbacks *cb, int view_radius_max);

/* Inject the persistence handle (persist.h Section 5; ARCHITECTURE Section 8).
 * OPTIONAL and separate from world_init so the init signature and every existing
 * caller stay unchanged: world_init leaves ws->store == NULL (pure regen), and a
 * caller that wants persisted edits opens a PersistStore (persist_open, seeded to
 * THIS world's seed) and hands it here before priming/streaming. Pass NULL to
 * detach (back to ephemeral regen). The WorldStore does NOT take ownership: the
 * caller still owns the handle's lifetime and is responsible for persist_close()
 * (world_shutdown only flushes/closes through it if the caller wires that). May
 * be called with a NULL ws (no-op). */
void world_set_persist(WorldStore *ws, PersistStore *store);

/* Evict every resident (returning all slabs + render slots), then free the
 * pool. Idempotent-safe to call once. */
void world_shutdown(WorldStore *ws);

/* ======================================================================== *
 *  9. RESIDENCY API  (cold path - insert/evict/lookup; Section 2.5)         *
 * ======================================================================== */
/* Hash lookup: the resident Chunk at (cx,cy,cz), or NULL if not resident.
 * The COLD path (Section 2.5): hot neighbour traversal uses Chunk.neigh[6],
 * not this. O(1) average (linear probe < 1.3 at this load factor). */
Chunk *world_get(const WorldStore *ws, int cx, int cy, int cz);

/* Bring (cx,cy,cz) resident: pop a slab, set its coords, call cb.gen to fill it
 * from seed, insert into the hash + resident list, wire its neigh[6] to any
 * resident neighbours AND patch those neighbours' back-pointers, assign a render
 * slot, then queue ITSELF and any boundary neighbour whose seam changed for
 * (re)mesh. Returns the new Chunk*, or NULL if already resident or the pool is
 * full (a pool-full return is a hard invariant violation - see tests). The
 * actual meshing happens in the budgeted drain (world_stream_update), NOT here,
 * so a batch insert does not blow the frame. */
Chunk *world_insert(WorldStore *ws, int cx, int cy, int cz);

/* Evict the chunk at (cx,cy,cz): NULL it out of its 6 resident neighbours'
 * neigh[] back-pointers (and queue THOSE neighbours for remesh - their seam
 * faces re-open), remove it from the hash (backward-shift to stay tombstone-
 * free), drop it from the resident list (O(1) swap-remove), free its render
 * slot (cb.slot_free), and return its slab to the pool free stack. No disk I/O
 * this milestone (Section 8 deferred): a MODIFIED chunk loses its edits here -
 * documented and acceptable. No-op if not resident. */
void world_evict(WorldStore *ws, int cx, int cy, int cz);

/* ---- sparse-air realize / uniform (0.5 M1) ---- *
 * world_realize: ensure a resident chunk has a real 16 KiB voxel block (pops one
 * from the slab sub-pool if the chunk is currently uniform). Returns 0 on
 * success, non-zero if the slab pool is exhausted (a sizing bug). A no-op if the
 * chunk is already realized. Call BEFORE any write to c->voxels (worldgen fill,
 * persist load, the CA binding a chunk, a player edit).
 *   world_set_uniform: collapse a chunk to a single repeated word (e.g. all-air),
 * returning its slab to the pool if it had one. voxels becomes NULL; reads go
 * through chunk_vox() which returns uniform_word. */
int  world_realize(WorldStore *ws, Chunk *c);
void world_set_uniform(WorldStore *ws, Chunk *c, Voxel word);

/* Iterate resident chunks. The frame loop walks these to draw; the streaming
 * update walks them to find out-of-range evictees. world_resident_count() is
 * the live count (<= WORLD_WINDOW_CHUNKS under the distance policy, the bounded
 * invariant the tests assert). world_resident_at(i) returns the i-th resident
 * Chunk* (i in [0, count)); order is unspecified and changes across evicts
 * (swap-remove), so callers must re-read count each pass. */
uint32_t world_resident_count(const WorldStore *ws);
Chunk   *world_resident_at(const WorldStore *ws, uint32_t i);

/* The render slot a resident chunk currently owns (for the draw loop), or -1
 * if none (only transiently, e.g. a headless store with no slot allocator). */
int  world_render_slot(const WorldStore *ws, const Chunk *c);

/* ======================================================================== *
 *  10. VOXEL EDIT API  (player block break / place; 0.2)                    *
 * ======================================================================== */
/* Read the voxel at WORLD voxel coordinate (wx,wy,wz). Returns 0 (MAT_AIR) when
 * the containing chunk is not resident, so a raycast treats unloaded space as
 * empty. Read-only; the COLD residency hash, not the neigh fast-path. */
Voxel world_get_voxel(const WorldStore *ws, int wx, int wy, int wz);

/* Set the voxel at WORLD voxel coordinate (wx,wy,wz) and do all the bookkeeping a
 * player edit needs: flag the chunk CHUNK_MODIFIED (so the edit persists on
 * eviction) and CHUNK_DIRTY_MESH + enqueue it for remesh, and - if the voxel sits
 * on a chunk-boundary plane - dirty + enqueue the abutting resident neighbour(s)
 * so the shared seam re-culls. The actual relight+remesh+upload happens in the
 * next world_stream_update drain. Returns 1 if edited, 0 if the chunk is not
 * resident (an edit to unloaded space is dropped - the caller should only edit
 * voxels the raycast actually hit, which are by definition resident). Does NOT
 * touch the simulation; the caller wakes the sim via sim_notify_edit when the
 * edited chunk is the simulated one (the world layer has no sim dependency). */
int  world_edit_voxel(WorldStore *ws, int wx, int wy, int wz, Voxel v);

/* Reset a resident chunk's voxels to its pure seed (regenerate via cb.gen) and
 * queue a FULL remesh of it + its 6 resident neighbours (a whole-chunk change moves
 * every seam). No-op if the chunk is not resident. Used by the MP client's chunk-
 * delta apply: a streamed delta is delta-from-seed, so the client must start each
 * apply from seed - otherwise cells the host reverted to seed-equal (and therefore
 * OMITTED from the delta) would linger forever (the additive-apply desync). */
void world_reset_to_seed(WorldStore *ws, int cx, int cy, int cz);

/* ======================================================================== *
 *  10. STREAMING DRIVER  (the per-frame entry point - Section 6/7)          *
 * ======================================================================== */
/* Move the loaded window to centre on world position (player_x, player_z), then
 * drain a BUDGETED slice of pending gen + remesh work. Call ONCE per frame from
 * the main loop, before drawing.
 *
 * Steps (Section 7 eviction/streaming rule):
 *   1. Convert player_x/z to a chunk (cx,cz) = floor(world / 16). If unchanged
 *      since last frame AND no work is pending, do only step 4 (drain) and
 *      return - a stationary player costs nothing.
 *   2. EVICT every resident whose (cx,cz) Chebyshev distance from the new
 *      centre exceeds WORLD_RADIUS (trailing edge). world_evict each.
 *   3. ENQUEUE for generation every (cx,cy,cz) in the new window's band that is
 *      not already resident (leading edge). They are not generated yet - just
 *      queued - so a big move spreads over frames.
 *   4. DRAIN the queues under budget: pop up to WORLD_GEN_BUDGET gen requests
 *      (each: world_insert -> pop slab, gen, link, queue self+neighbours for
 *      remesh) and up to WORLD_REMESH_BUDGET remesh requests (each: cb.mesh_
 *      upload on the chunk, clear its CHUNK_DIRTY_MESH). Leftover work stays
 *      queued for the next frame.
 *
 * 0.5 M2: player_y now matters - the 9-layer vertical band re-anchors on the
 * player's chunk-Y (the planet's surface sits at high Y at R=512), so all three
 * coords drive the resident window. */
void world_stream_update(WorldStore *ws, float player_x, float player_y, float player_z);

/* Set the active view distance (Chebyshev window radius, chunks): clamped to
 * [2, WORLD_RADIUS]. Takes effect on the next world_stream_update (the window grows
 * or shrinks to the new radius, streaming/evicting the difference under budget). The
 * reserved pools are sized for WORLD_RADIUS, so this never reallocates. Returns the
 * clamped radius actually set. */
int  world_set_view_radius(WorldStore *ws, int radius);

/* The active view radius (chunks). */
int  world_view_radius(const WorldStore *ws);

/* Force-fill the ENTIRE window around (player_x, player_y, player_z) synchronously,
 * ignoring the per-frame budget: generate + mesh every in-range chunk now.
 * Used at startup (so the first frame shows full terrain, like the old static
 * grid) and in tests (deterministic, no frame pacing). Equivalent to calling
 * world_stream_update and draining to empty. */
void world_prime(WorldStore *ws, float player_x, float player_y, float player_z);

/* ======================================================================== *
 *  11. NEIGHBOUR MAINTENANCE  (M5 seamless meshing as the window slides)    *
 * ======================================================================== *
 * Exposed for tests; normally driven internally by insert/evict. Re-derives
 * chunk c's neigh[6] from the current hash (each of the 6 axis-adjacent coords
 * -> world_get) AND writes the reciprocal back-pointer on each resident
 * neighbour (neighbour.neigh[opposite(dir)] = c). A NULL (non-resident)
 * neighbour leaves that slot NULL, so an edge-of-window chunk meshes its outer
 * faces exactly like an isolated chunk - preserving the M5 rule. Returns the
 * number of resident neighbours wired (0..6). The opposite-face helper matches
 * the Face enum: opposite(NEG_X)=POS_X, etc. (dir ^ 1). */
int  world_wire_neighbours(WorldStore *ws, Chunk *c);

#endif /* WORLD_H */
