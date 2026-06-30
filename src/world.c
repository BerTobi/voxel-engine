/* world.c - The WorldStore: resident-chunk registry, slab pool, and the
 * player-centred streaming window. The runtime implementation of the contract
 * in world.h (ARCHITECTURE.md Section 1.5 ownership, 2.5 open-addressing chunk
 * hash + 21-bit packed key + cached-neigh inner loop, Section 7 loaded window /
 * distance eviction / fixed slab pool / no per-chunk malloc, Section 6 budgeted
 * per-frame work drain).
 *
 * Pure C99, single-threaded. NO GL and NO OS here: worldgen, meshing, and the
 * render-slot upload are reached only through the WorldCallbacks function
 * pointers handed in at world_init, so this file builds and unit-tests headless
 * (test_world.c passes NULL render callbacks). Memory is bounded: every array
 * is fixed-size on the struct and the slab pool is one calloc that never grows.
 *
 * Persistence of MODIFIED chunks (Section 8) is now WIRED through an OPTIONAL,
 * injected PersistStore* (ws->store, set via world_set_persist after init; NULL
 * == disabled == today's pure-regen behaviour). The three hooks the design
 * reserves are now live:
 *   - world_insert / LOAD : try persist_load_chunk FIRST; on a hit the chunk's
 *     mat|temp|fill are restored from disk and cb.gen is SKIPPED; on a miss (the
 *     common case, and always when store==NULL) fall through to cb.gen exactly
 *     as before. Either way the chunk is lit + meshed by the normal remesh queue.
 *   - world_evict / EVICT : a chunk carrying CHUNK_MODIFIED is written back via
 *     persist_save_chunk BEFORE its slab returns to the pool, so an edit survives
 *     the eviction that fires when the player roams away. An unmodified chunk is
 *     dropped with zero I/O (the seed is its storage).
 *   - world_shutdown / FLUSH : every resident is evicted (so every modified one
 *     is saved through the evict hook), then persist_flush lands the region
 *     headers/indices, so edits survive a process restart, not just an eviction.
 * The WorldStore does NOT own the handle (persist_close is the caller's job);
 * worldgen stays deterministic, so an UNMODIFIED chunk still costs zero bytes.
 */
#include <stdlib.h>
#include <string.h>

#include "world.h"
#include "persist.h"   /* PersistStore + persist_load/save/flush (Section 8).
                        * world.h forward-declares PersistStore and stays
                        * persist.h-free; world.c is the one place that actually
                        * calls the persist_* API, so the on-disk format leaks no
                        * further than this translation unit. A NULL ws->store
                        * (the default after world_init's memset, and what
                        * test_world.c always runs with) makes every persist call
                        * a documented no-op, so the streaming/regen behaviour is
                        * byte-for-byte unchanged when persistence is disabled. */

/* ======================================================================== *
 *  Small helpers                                                           *
 * ======================================================================== */

/* Pool index of a resident chunk: its offset into the contiguous record array.
 * Valid only for a chunk that came from ws->pool (every resident chunk does). */
static inline uint32_t pool_index(const WorldStore *ws, const Chunk *c)
{
    return (uint32_t)(c - ws->pool);
}

/* ---- sparse-air slab sub-pool (0.5 M1) ---------------------------------- *
 * slab_pop hands a RAW (unfilled) 16 KiB voxel block to a chunk; world_realize
 * (public) additionally expands the chunk's uniform_word into it. Returns 0 on
 * success, 1 if the slab pool is exhausted (a WORLD_SLAB_SLOTS sizing bug). */
static int slab_pop(WorldStore *ws, Chunk *c)
{
    uint32_t sidx, inuse;
    if (c->voxels != NULL)
        return 0;                            /* already realized */
    if (ws->slab_free_top == 0u)
        return 1;                            /* exhausted: caller handles */
    sidx = ws->slab_free[--ws->slab_free_top];
    c->slab_idx = (int32_t)sidx;
    c->voxels   = ws->slabs + (size_t)sidx * CHUNK_VOXELS;
    c->flags   &= ~(uint8_t)CHUNK_UNIFORM;
    inuse = ws->slab_slots - ws->slab_free_top;
    if (inuse > ws->slab_inuse_peak)
        ws->slab_inuse_peak = inuse;
    return 0;
}

int world_realize(WorldStore *ws, Chunk *c)
{
    Voxel u;
    int i;
    if (c == NULL)
        return 1;
    if (c->voxels != NULL)
        return 0;                            /* already realized */
    u = c->uniform_word;                     /* capture before slab_pop clears state */
    if (slab_pop(ws, c) != 0)
        return 1;
    /* Expand the uniform value into the fresh (possibly recycled, stale) block so
     * every voxel the caller does NOT overwrite is correct - the realize-on-EDIT
     * and realize-on-CA-wake of a uniform-air chunk must read air everywhere else.
     * (world_insert uses slab_pop directly + overwrites all 4096, so it pays no
     * redundant fill.) */
    for (i = 0; i < CHUNK_VOXELS; ++i)
        c->voxels[i] = u;
    return 0;
}

void world_set_uniform(WorldStore *ws, Chunk *c, Voxel word)
{
    if (c == NULL)
        return;
    if (c->slab_idx >= 0) {                  /* return its block to the sub-pool */
        ws->slab_free[ws->slab_free_top++] = (uint32_t)c->slab_idx;
        c->slab_idx = -1;
    }
    c->voxels       = NULL;
    c->uniform_word = word;
    c->flags       |= CHUNK_UNIFORM;
}

/* Chebyshev (chessboard) distance between two chunk columns, used by the
 * window/eviction policy. */
static inline int cheby(int ax, int az, int bx, int bz)
{
    int dx = ax - bx; if (dx < 0) dx = -dx;
    int dz = az - bz; if (dz < 0) dz = -dz;
    return (dx > dz) ? dx : dz;
}

/* The 6 axis-adjacent unit steps in canonical Face order
 * (0=-X,1=+X,2=-Y,3=+Y,4=-Z,5=+Z), matching chunk.h's neigh[] layout. The
 * opposite of dir is (dir ^ 1) (NEG/POS pairing). */
static const int NEIGH_DX[6] = { -1, +1,  0,  0,  0,  0 };
static const int NEIGH_DY[6] = {  0,  0, -1, +1,  0,  0 };
static const int NEIGH_DZ[6] = {  0,  0,  0,  0, -1, +1 };

/* ======================================================================== *
 *  Residency hash (Section 2.5): open-addressing, linear probe, no         *
 *  tombstones (backward-shift deletion).                                   *
 * ======================================================================== */

/* A slot is EMPTY iff ptr==NULL (key==0 alone is ambiguous because chunk
 * (0,0,0) packs to key 0; the ptr disambiguates - so we test ptr). */
static inline int slot_empty(const ChunkSlot *s)
{
    return s->ptr == NULL;
}

/* Find the resident Chunk at (cx,cy,cz), or NULL. Cold path only. */
Chunk *world_get(const WorldStore *ws, int cx, int cy, int cz)
{
    uint64_t key = chunk_key(cx, cy, cz);
    uint32_t mask = WORLD_HASH_CAP - 1u;
    uint32_t i = key_hash(key) & mask;

    /* Linear probe until an empty slot (key absent) or a key match. The table
     * is kept well under load 0.7 and tombstone-free, so this terminates. */
    for (;;) {
        const ChunkSlot *s = &ws->table[i];
        if (slot_empty(s))
            return NULL;                 /* hit an empty slot -> not resident */
        if (s->key == key && s->ptr != NULL)
            return s->ptr;
        i = (i + 1u) & mask;
    }
}

/* Insert (key,ptr) into the table. Caller guarantees the key is not already
 * present and the table has room (load factor invariant). */
static void table_insert(WorldStore *ws, uint64_t key, Chunk *c)
{
    uint32_t mask = WORLD_HASH_CAP - 1u;
    uint32_t i = key_hash(key) & mask;

    while (!slot_empty(&ws->table[i]))
        i = (i + 1u) & mask;

    ws->table[i].key = key;
    ws->table[i].ptr = c;
    ws->table_count++;
}

/* Remove `key` and close the probe chain via backward-shift (Robin-Hood-for-
 * linear-probing) deletion, so the table stays tombstone-free across an
 * unbounded number of insert/evict cycles. Standard linear-probing deletion
 * with an explicit hole index and a scan cursor: after clearing the matched
 * slot, pull forward any later entry that probed past the hole and whose home
 * is at-or-before the hole (so it stays reachable by a forward probe). */
static void table_delete(WorldStore *ws, uint64_t key)
{
    uint32_t mask = WORLD_HASH_CAP - 1u;
    uint32_t i = key_hash(key) & mask;
    uint32_t hole;

    /* locate the key */
    for (;;) {
        ChunkSlot *s = &ws->table[i];
        if (slot_empty(s))
            return;                      /* not present (defensive) */
        if (s->key == key && s->ptr != NULL)
            break;
        i = (i + 1u) & mask;
    }
    hole = i;

    /* clear it */
    ws->table[hole].key = 0;
    ws->table[hole].ptr = NULL;
    ws->table_count--;

    /* backward-shift: pull forward any entry that probed past `hole` */
    uint32_t j = (hole + 1u) & mask;
    for (;;) {
        ChunkSlot *s = &ws->table[j];
        if (slot_empty(s))
            break;                       /* end of cluster */

        uint32_t home = key_hash(s->key) & mask;
        /* Can the entry at j legally occupy `hole`? It can iff `hole` lies in
         * the cyclic interval [home .. j], i.e. moving it back keeps it
         * reachable by a forward probe from its home. */
        uint32_t dist_j    = (j    - home) & mask;   /* how far j is past home */
        uint32_t dist_hole = (hole - home) & mask;   /* how far hole is past home */

        if (dist_hole <= dist_j) {
            /* relocate j into the hole, open a new hole at j */
            ws->table[hole] = ws->table[j];
            ws->table[j].key = 0;
            ws->table[j].ptr = NULL;
            hole = j;
        }
        j = (j + 1u) & mask;
    }
}

/* ======================================================================== *
 *  Resident iteration list (dense pool indices + O(1) swap-remove)         *
 * ======================================================================== */

static void resident_add(WorldStore *ws, uint32_t pidx)
{
    ws->resident_at[pidx] = ws->resident_count;
    ws->resident[ws->resident_count] = pidx;
    ws->resident_count++;
}

static void resident_remove(WorldStore *ws, uint32_t pidx)
{
    uint32_t at   = ws->resident_at[pidx];
    uint32_t last = ws->resident_count - 1u;

    if (at != last) {
        uint32_t moved = ws->resident[last];
        ws->resident[at]       = moved;
        ws->resident_at[moved] = at;     /* the moved chunk's new position */
    }
    ws->resident_count = last;
}

uint32_t world_resident_count(const WorldStore *ws)
{
    return ws->resident_count;
}

Chunk *world_resident_at(const WorldStore *ws, uint32_t i)
{
    if (i >= ws->resident_count)
        return NULL;
    return &ws->pool[ws->resident[i]];
}

int world_render_slot(const WorldStore *ws, const Chunk *c)
{
    return ws->render_slot[pool_index(ws, c)];
}

/* ======================================================================== *
 *  Remesh queue (pool indices, deduped by CHUNK_DIRTY_MESH)                *
 * ======================================================================== */

/* Queue chunk c for (re)mesh, unless it is already dirty/queued. The
 * CHUNK_DIRTY_MESH flag is the dedup key: a chunk touched by several neighbour
 * changes in one frame is enqueued at most once. Capacity == WORLD_POOL_SLOTS,
 * which is the max number of distinct live chunks, so the ring never overflows
 * (each pool index appears at most once at a time). */
static void remesh_enqueue(WorldStore *ws, Chunk *c)
{
    uint32_t next;
    if (c->flags & CHUNK_DIRTY_MESH)
        return;                          /* already queued (the dedup key) */
    /* Ring-full guard (defense-in-depth, like gen_enqueue): the CHUNK_DIRTY_MESH
     * dedup already caps live entries at WORLD_POOL_SLOTS == WORLD_REMESHQ_CAP so
     * this never fires, but enforcing it means a lapped ring can't silently drop
     * a remesh. If it WOULD overflow, keep the flag clear so a later enqueue can
     * still queue the chunk. */
    next = (ws->remeshq_tail + 1u) % WORLD_REMESHQ_CAP;
    if (next == ws->remeshq_head)
        return;                          /* ring full: drop (invariant: unreachable) */
    c->flags |= CHUNK_DIRTY_MESH;
    ws->remeshq[ws->remeshq_tail] = pool_index(ws, c);
    ws->remeshq_tail = next;
}

static int remesh_empty(const WorldStore *ws)
{
    return ws->remeshq_head == ws->remeshq_tail;
}

/* ======================================================================== *
 *  Gen queue (coords to generate; bounded ring)                            *
 * ======================================================================== */

static void gen_enqueue(WorldStore *ws, int cx, int cy, int cz)
{
    /* Ring-full guard: a head==tail ring cannot tell "full" from "empty", so if
     * the tail were ever allowed to lap the head, gen_empty() would read EMPTY
     * and silently drop every still-queued coord. enqueue_in_range now rebuilds
     * the queue from empty each move and pushes at most WORLD_WINDOW_CHUNKS (338)
     * distinct coords < WORLD_GENQ_CAP (402), so this can't fire - it enforces
     * world.h's "queues never overflow" invariant rather than assuming it. */
    uint32_t next = (ws->genq_tail + 1u) % WORLD_GENQ_CAP;
    if (next == ws->genq_head)
        return;                          /* ring full: drop (invariant: unreachable) */
    ws->genq_cx[ws->genq_tail] = cx;
    ws->genq_cy[ws->genq_tail] = cy;
    ws->genq_cz[ws->genq_tail] = cz;
    ws->genq_tail = next;
}

static int gen_empty(const WorldStore *ws)
{
    return ws->genq_head == ws->genq_tail;
}

/* ======================================================================== *
 *  Neighbour maintenance (M5 seamless meshing as the window slides)        *
 * ======================================================================== */

/* Re-derive c->neigh[6] from the current hash and write the reciprocal
 * back-pointer on every resident neighbour. A newly-wired neighbour has its
 * seam toward c re-culled, so it is queued for remesh. A non-resident neighbour
 * leaves neigh[dir]=NULL (mesher treats that boundary as AIR). Returns the
 * number of resident neighbours wired (0..6). */
int world_wire_neighbours(WorldStore *ws, Chunk *c)
{
    int dir;
    int wired = 0;

    for (dir = 0; dir < 6; ++dir) {
        Chunk *n = world_get(ws,
                             c->cx + NEIGH_DX[dir],
                             c->cy + NEIGH_DY[dir],
                             c->cz + NEIGH_DZ[dir]);
        c->neigh[dir] = n;
        if (n != NULL) {
            n->neigh[dir ^ 1] = c;       /* reciprocal back-pointer */
            remesh_enqueue(ws, n);       /* its seam toward c now culls */
            wired++;
        }
    }
    return wired;
}

/* On evict: NULL the evictee out of each resident neighbour's back-pointer and
 * queue those neighbours for remesh (their seam faces toward c re-open). */
static void unwire_neighbours(WorldStore *ws, Chunk *c)
{
    int dir;
    for (dir = 0; dir < 6; ++dir) {
        Chunk *n = c->neigh[dir];
        if (n != NULL) {
            n->neigh[dir ^ 1] = NULL;
            remesh_enqueue(ws, n);
            c->neigh[dir] = NULL;
        }
    }
}

/* ======================================================================== *
 *  Render-slot allocation (free stack)                                     *
 * ======================================================================== */

static int slot_acquire(WorldStore *ws)
{
    if (ws->slot_free_top == 0u)
        return -1;                       /* none free (window << slots, can't happen) */
    return ws->slot_free[--ws->slot_free_top];
}

static void slot_release(WorldStore *ws, int slot)
{
    if (slot < 0)
        return;
    ws->slot_free[ws->slot_free_top++] = slot;
}

/* ======================================================================== *
 *  Insert / Evict                                                          *
 * ======================================================================== */

Chunk *world_insert(WorldStore *ws, int cx, int cy, int cz)
{
    uint32_t pidx;
    Chunk   *c;
    int      slot;

    if (world_get(ws, cx, cy, cz) != NULL)
        return NULL;                     /* already resident */
    if (ws->free_top == 0u)
        return NULL;                     /* pool full: hard invariant violation */

    /* pop a slab */
    pidx = ws->free_idx[--ws->free_top];
    c = &ws->pool[pidx];

    /* clear the record so a recycled slot carries no stale neigh/flags before
     * worldgen stamps it. memset leaves voxels==NULL/uniform_word=0 (uniform-air);
     * restate slab_idx == -1 (0 would alias slab 0). */
    memset(c, 0, sizeof(*c));
    c->slab_idx = -1;
    c->cx = cx;
    c->cy = cy;
    c->cz = cz;

    /* 0.5 M1: hand the chunk a RAW voxel block, then fill it. persist HIT or gen
     * overwrites all 4096 (so slab_pop does no redundant fill). A gen MISS that
     * comes out WHOLLY AIR is collapsed back to uniform-air (slab returned), which
     * is the 72%-of-the-window memory win. Slab exhaustion is a sizing bug
     * (WORLD_SLAB_SLOTS); roll the record pop back and fail like a full pool. */
    if (slab_pop(ws, c) != 0) {
        ws->free_idx[ws->free_top++] = pidx;
        return NULL;
    }

    /* Section 8 gen-vs-stored: PERSISTED edits win over the seed. Try the save
     * first - persist_load_chunk restores a stored chunk's mat|temp|fill verbatim
     * (light|ao|flags left 0 for light.c / the CA to rebake / re-wake) and returns
     * 1 on a HIT, skipping the generator. The common case (and ALWAYS when
     * ws->store == NULL, e.g. test_world.c) is a MISS, so we gen exactly as before.
     * A store-less or is_air-less store keeps every chunk realized (dense), so the
     * pre-0.5 behaviour is byte-for-byte preserved. */
    if (!persist_load_chunk(ws->store, c, cx, cy, cz)) {
        ws->cb.gen(c, cx, cy, cz, ws->seed, ws->cb.user);
        if (ws->cb.is_air && ws->cb.is_air(cx, cy, cz, ws->seed, ws->cb.user))
            /* Collapse to uniform using the generator's OWN air word (voxels[0],
             * which the all-air gen made identical everywhere) - NOT a bare 0 - so
             * the uniform value carries worldgen's ambient temp/fill exactly. A
             * neighbour CA reading this chunk via chunk_vox() then sees the same
             * temperature it would from a dense air voxel: byte-identical behaviour. */
            world_set_uniform(ws, c, c->voxels[0]);   /* wholly air -> drop the slab */
    }
    /* gen sets CHUNK_GEN|CHUNK_DIRTY_MESH; a persist HIT leaves flags 0 (the
     * remesh enqueue below raises CHUNK_DIRTY_MESH unconditionally, so a loaded
     * chunk still meshes). Restate coords either way in case a test gen callback,
     * or the persist loader, did not. */
    c->cx = cx;
    c->cy = cy;
    c->cz = cz;

    /* register residency BEFORE wiring neighbours (so a neighbour's world_get
     * for THIS chunk would find it, and so the reciprocal wiring is symmetric). */
    table_insert(ws, chunk_key(cx, cy, cz), c);
    resident_add(ws, pidx);

    /* assign a render slot for the chunk's residency lifetime */
    slot = slot_acquire(ws);
    ws->render_slot[pidx] = slot;

    /* wire neigh[6] both directions; queues touched neighbours for remesh */
    world_wire_neighbours(ws, c);

    /* queue this chunk itself for (re)mesh. worldgen already raised
     * CHUNK_DIRTY_MESH, so push it onto the queue explicitly (without the
     * flag-flip that remesh_enqueue's dedup would skip). */
    if (!(c->flags & CHUNK_DIRTY_MESH))
        c->flags |= CHUNK_DIRTY_MESH;
    {
        /* Same ring-full guard as remesh_enqueue (this push bypasses its dedup
         * because worldgen already raised CHUNK_DIRTY_MESH on this fresh slab). */
        uint32_t next = (ws->remeshq_tail + 1u) % WORLD_REMESHQ_CAP;
        if (next != ws->remeshq_head) {
            ws->remeshq[ws->remeshq_tail] = pidx;
            ws->remeshq_tail = next;
        }
    }

    return c;
}

void world_evict(WorldStore *ws, int cx, int cy, int cz)
{
    Chunk   *c = world_get(ws, cx, cy, cz);
    uint32_t pidx;
    int      slot;

    if (c == NULL)
        return;                          /* not resident: no-op */

    pidx = pool_index(ws, c);

    /* Section 8 writeback (now LIVE): a player-MODIFIED chunk is persisted
     * BEFORE its slab is returned to the pool (and before c->flags is cleared
     * below), so an edit survives the eviction that fires the instant the player
     * roams out of range. persist_save_chunk canonicalises to mat|temp|fill,
     * palettizes + RLE-compresses, and writes it into the region file. An
     * UNMODIFIED chunk is dropped with ZERO I/O - the deterministic seed is its
     * storage (gen-vs-stored). ws->store == NULL (persistence disabled) makes
     * this a no-op, so the M7 ephemeral-regen behaviour is exactly preserved.
     * A failed save is logged inside persist.c and returns non-zero; we drop the
     * slab anyway - a disk error must NOT wedge eviction or grow the window. */
    if ((c->flags & CHUNK_MODIFIED) && ws->store != NULL)
        (void)persist_save_chunk(ws->store, c);

    /* NULL the evictee out of its resident neighbours and queue them to remesh
     * (their seams toward c re-open). */
    unwire_neighbours(ws, c);

    /* remove from the hash (backward-shift, tombstone-free) */
    table_delete(ws, chunk_key(cx, cy, cz));

    /* drop from the resident list (O(1) swap-remove) */
    resident_remove(ws, pidx);

    /* free the render slot (mark drawable-as-nothing) */
    slot = ws->render_slot[pidx];
    if (slot >= 0) {
        if (ws->cb.slot_free)
            ws->cb.slot_free(slot, ws->cb.user);
        slot_release(ws, slot);
        ws->render_slot[pidx] = -1;
    }

    /* If this chunk was sitting in the remesh queue (dirty), clear its dirty
     * flag so the drain skips the now-dead pool index (the drain re-checks the
     * flag and residency, but clearing keeps the invariant that a dead slab is
     * never "dirty"). */
    c->flags = 0;

    /* 0.5 M1: return the voxel block (if realized) to the slab sub-pool. A uniform
     * chunk (slab_idx == -1) borrowed none. Done AFTER the persist writeback above,
     * which only fires for CHUNK_MODIFIED chunks - and those are always realized
     * (world_edit_voxel realizes before it raises MODIFIED), so the save never sees
     * NULL voxels. */
    if (c->slab_idx >= 0) {
        ws->slab_free[ws->slab_free_top++] = (uint32_t)c->slab_idx;
        c->slab_idx = -1;
    }
    c->voxels = NULL;

    /* return the chunk record to the pool */
    ws->free_idx[ws->free_top++] = pidx;
}

/* ======================================================================== *
 *  Voxel edit API (player break/place; Section 10)                          *
 * ======================================================================== */

/* WORLD voxel coord -> chunk coord (floor) + local index 0..CHUNK_DIM-1. Uses
 * div/mod with a floor correction rather than a shift, so it is correct for
 * NEGATIVE world coords (e.g. w=-1 -> chunk -1, local CHUNK_DIM-1) without
 * relying on implementation-defined signed right-shift. */
static void wvox_to_chunk(int w, int *chunk, int *local)
{
    int c = w / CHUNK_DIM;
    int l = w % CHUNK_DIM;
    if (l < 0) { l += CHUNK_DIM; c -= 1; }   /* floor toward -inf */
    *chunk = c;
    *local = l;
}

Voxel world_get_voxel(const WorldStore *ws, int wx, int wy, int wz)
{
    int cx, cy, cz, lx, ly, lz;
    Chunk *c;
    wvox_to_chunk(wx, &cx, &lx);
    wvox_to_chunk(wy, &cy, &ly);
    wvox_to_chunk(wz, &cz, &lz);
    c = world_get(ws, cx, cy, cz);
    if (c == NULL)
        return 0;                    /* unloaded space reads as MAT_AIR */
    return chunk_get(c, lx, ly, lz);
}

int world_edit_voxel(WorldStore *ws, int wx, int wy, int wz, Voxel v)
{
    int cx, cy, cz, lx, ly, lz;
    Chunk *c, *n;
    wvox_to_chunk(wx, &cx, &lx);
    wvox_to_chunk(wy, &cy, &ly);
    wvox_to_chunk(wz, &cz, &lz);
    c = world_get(ws, cx, cy, cz);
    if (c == NULL)
        return 0;                    /* can't edit an unloaded chunk */

    /* 0.5 M1: a uniform-air chunk has no voxel block - realize it first (which
     * expands its uniform_word so every OTHER voxel stays correct air). */
    if (world_realize(ws, c) != 0)
        return 0;                    /* slab pool exhausted (sizing bug) */

    chunk_set(c, lx, ly, lz, v);      /* writes the voxel AND raises CHUNK_DIRTY_MESH */
    c->flags |= CHUNK_MODIFIED;       /* persist this edit on eviction */
    /* chunk_set already raised CHUNK_DIRTY_MESH, but remesh_enqueue's dedup SKIPS a
     * chunk that already reads dirty (the flag is its "already queued" marker). A
     * plain enqueue here would therefore set the flag yet never PUSH the chunk,
     * leaving it dirty-but-unqueued and never re-meshed - the edit lands in the
     * voxels (collision/raycast see it) but the mesh stays stale. Clear the flag
     * first so the enqueue actually queues it (same as world_insert's gen path). */
    c->flags &= ~(uint8_t)CHUNK_DIRTY_MESH;
    remesh_enqueue(ws, c);            /* now enqueues + re-raises CHUNK_DIRTY_MESH */

    /* A boundary-plane edit changes the shared seam, so the abutting resident
     * neighbour must re-mesh too (its faces toward this voxel may open/close). */
    if (lx == 0)             { n = world_get(ws, cx - 1, cy, cz); if (n) remesh_enqueue(ws, n); }
    if (lx == CHUNK_DIM - 1) { n = world_get(ws, cx + 1, cy, cz); if (n) remesh_enqueue(ws, n); }
    if (ly == 0)             { n = world_get(ws, cx, cy - 1, cz); if (n) remesh_enqueue(ws, n); }
    if (ly == CHUNK_DIM - 1) { n = world_get(ws, cx, cy + 1, cz); if (n) remesh_enqueue(ws, n); }
    if (lz == 0)             { n = world_get(ws, cx, cy, cz - 1); if (n) remesh_enqueue(ws, n); }
    if (lz == CHUNK_DIM - 1) { n = world_get(ws, cx, cy, cz + 1); if (n) remesh_enqueue(ws, n); }
    return 1;
}

void world_reset_to_seed(WorldStore *ws, int cx, int cy, int cz)
{
    Chunk *c = world_get(ws, cx, cy, cz);
    Chunk *n;
    if (c == NULL)
        return;                          /* not resident: nothing to reset */
    if (world_realize(ws, c) != 0)
        return;                          /* slab pool exhausted (cannot regen) */
    ws->cb.gen(c, cx, cy, cz, ws->seed, ws->cb.user);   /* pure seed terrain (now == seed) */
    c->flags &= ~(uint8_t)CHUNK_DIRTY_MESH;             /* clear so the enqueue actually queues */
    remesh_enqueue(ws, c);
    /* A whole-chunk change moves every shared seam: re-mesh all 6 resident neighbours. */
    n = world_get(ws, cx - 1, cy, cz); if (n) remesh_enqueue(ws, n);
    n = world_get(ws, cx + 1, cy, cz); if (n) remesh_enqueue(ws, n);
    n = world_get(ws, cx, cy - 1, cz); if (n) remesh_enqueue(ws, n);
    n = world_get(ws, cx, cy + 1, cz); if (n) remesh_enqueue(ws, n);
    n = world_get(ws, cx, cy, cz - 1); if (n) remesh_enqueue(ws, n);
    n = world_get(ws, cx, cy, cz + 1); if (n) remesh_enqueue(ws, n);
}

/* ======================================================================== *
 *  Lifecycle                                                               *
 * ======================================================================== */

/* Voxel-slab slots needed to cover a view-radius-r window PLUS a churn curtain of
 * slack (so a budgeted move never momentarily exceeds the pool), capped at the
 * compile-ceiling index-array size. The single allocation that scales with view
 * distance; shared by world_init (initial size) and world_set_view_radius (grow). */
static uint32_t world_slab_slots_for(int r)
{
    long diam = 2L * (long)r + 1;
    long need = diam * diam * WORLD_BAND_H + 2L * diam * WORLD_BAND_H;  /* window + 2 curtains */
    if (need > (long)WORLD_SLAB_SLOTS) need = (long)WORLD_SLAB_SLOTS;
    if (need < 1) need = 1;
    return (uint32_t)need;
}

int world_init(WorldStore *ws, uint64_t seed, const WorldCallbacks *cb, int view_radius_max)
{
    uint32_t i, slab_slots;

    if (ws == NULL || cb == NULL || cb->gen == NULL)
        return 1;                        /* gen is REQUIRED */

    memset(ws, 0, sizeof(*ws));
    ws->seed = seed;
    ws->cb   = *cb;

    /* Clamp the session view-distance ceiling to [2, WORLD_RADIUS] and size the VOXEL
     * slab pool to ITS window (+ a churn curtain of slack), NOT the compile ceiling -
     * so a small view distance reserves little and only a 256 m session asks for the
     * ~640 MiB. The active radius (below) starts smaller and is clamped to this. */
    if (view_radius_max < 2)            view_radius_max = 2;
    if (view_radius_max > WORLD_RADIUS) view_radius_max = WORLD_RADIUS;
    ws->view_radius_max = view_radius_max;
    slab_slots = world_slab_slots_for(view_radius_max);
    ws->slab_slots = slab_slots;

    /* one big contiguous CHUNK-RECORD pool, reserved once and never grown.
     * 0.5 M1: a record no longer carries an inline 16 KiB voxel block, so this is
     * small even at the ceiling (records are ~100 B). */
    ws->pool = (Chunk *)calloc(WORLD_POOL_SLOTS, sizeof(Chunk));
    if (ws->pool == NULL)
        return 1;

    /* 0.5 M1: the separate VOXEL-BLOCK slab sub-pool (one contiguous calloc, never
     * grows) + its free-list. 0.5 (256 m): sized to ws->slab_slots (the session view-
     * distance ceiling), so this is the one allocation that scales with view distance.
     * Realized (non-uniform) chunks borrow a block; uniform-air ones borrow none.
     * calloc-zero = a block of MAT_AIR. A NULL here = the chosen view distance needs
     * more RAM than is available (the caller reports it + suggests a smaller one). */
    ws->slabs = (Voxel *)calloc((size_t)slab_slots * CHUNK_VOXELS, sizeof(Voxel));
    if (ws->slabs == NULL) {
        free(ws->pool);
        ws->pool = NULL;
        return 1;
    }
    ws->slab_free_top = 0;
    for (i = slab_slots; i-- > 0; )
        ws->slab_free[ws->slab_free_top++] = i;
    ws->slab_inuse_peak = 0;

    /* free-list stack: all slabs free (push in descending order so the first
     * pop hands out index 0 - tidy, not required). */
    ws->free_top = 0;
    for (i = WORLD_POOL_SLOTS; i-- > 0; )
        ws->free_idx[ws->free_top++] = i;

    /* render-slot free stack: all slots free (descending so slot 0 hands out
     * first). */
    ws->slot_free_top = 0;
    for (i = MAX_RENDER_CHUNKS; i-- > 0; )
        ws->slot_free[ws->slot_free_top++] = (int)i;

    /* render_slot defaults to -1 (no slot) for every pool index */
    for (i = 0; i < WORLD_POOL_SLOTS; ++i)
        ws->render_slot[i] = -1;

    /* queues empty, hash empty (memset already), no center yet. ws->store is
     * NULL (memset) == persistence disabled until world_set_persist injects a
     * handle, so an unaugmented caller (test_world.c) runs pure regen. */
    ws->genq_head = ws->genq_tail = 0;
    ws->remeshq_head = ws->remeshq_tail = 0;
    ws->have_center = 0;
    /* Start the ACTIVE radius at the session ceiling (so an explicit view distance
     * takes effect immediately, incl. headless captures). The caller may lower it
     * afterwards via world_set_view_radius (main.c does, for the no-env default, so a
     * plain launch starts light and the player raises it with Up/Down). */
    ws->view_radius = view_radius_max;

    return 0;
}

/* Set the active view radius. GROWS the slab pool ON DEMAND: a request beyond the
 * current ceiling (ws->view_radius_max) reallocates ws->slabs bigger to fit it (up to
 * the compile WORLD_RADIUS), so the player can keep raising view distance with no fixed
 * cap - until a realloc fails (out of RAM), at which point the radius stays where it is
 * (the hardware decides the limit, not an arbitrary number). The pool is grow-ONLY
 * (lowering the radius keeps the larger pool, so the player can raise it again free).
 *
 * Growing reallocs the one big voxel block, which MAY MOVE it, so every realized
 * resident chunk's voxels pointer is re-derived from its (stable) slab index against
 * the new base. Safe because this is called between frames (from the input handler):
 * the sim/mesher re-read chunk->voxels each tick, never caching a raw slab pointer.
 *
 * The change is picked up by the next world_stream_update (have_center=0 forces a full
 * window re-evaluation under the normal per-frame budget). Returns the radius actually
 * set (== the request, or the unchanged ceiling if the grow ran out of memory). */
int world_set_view_radius(WorldStore *ws, int radius)
{
    if (ws == NULL) return 0;
    if (radius < 2)            radius = 2;
    if (radius > WORLD_RADIUS) radius = WORLD_RADIUS;   /* hard compile ceiling (index arrays) */

    if (radius > ws->view_radius_max) {                 /* need a bigger slab pool */
        uint32_t need = world_slab_slots_for(radius);
        if (need > ws->slab_slots) {
            Voxel *p = (Voxel *)realloc(ws->slabs,
                                        (size_t)need * CHUNK_VOXELS * sizeof(Voxel));
            if (p == NULL) {
                /* Out of memory: cannot grow. Hold the radius at the current ceiling
                 * (the slab pool is unchanged + still valid). */
                radius = ws->view_radius_max;
            } else {
                if (p != ws->slabs) {                   /* block moved: rebase live voxels */
                    uint32_t k;
                    ws->slabs = p;
                    for (k = 0; k < ws->resident_count; ++k) {
                        Chunk *c = &ws->pool[ws->resident[k]];
                        if (c->voxels != NULL && c->slab_idx >= 0)
                            c->voxels = ws->slabs
                                      + (size_t)c->slab_idx * CHUNK_VOXELS;
                    }
                }
                /* The newly-added slabs [old..need) join the free stack. (Raw/uninit
                 * is fine: world_realize fills a popped slab before any read.) */
                { uint32_t s; for (s = need; s-- > ws->slab_slots; )
                    ws->slab_free[ws->slab_free_top++] = s; }
                ws->slab_slots = need;
                ws->view_radius_max = radius;
            }
        } else {
            ws->view_radius_max = radius;               /* pool already covers it */
        }
    }

    if (radius != ws->view_radius) {
        ws->view_radius = radius;
        ws->have_center = 0;        /* force a full window re-evaluation next stream */
    }
    return ws->view_radius;
}

int world_view_radius(const WorldStore *ws)
{
    return (ws != NULL) ? ws->view_radius : WORLD_VIEW_RADIUS_DEFAULT;
}

/* Inject (or detach with NULL) the optional persistence handle. Separate from
 * world_init so the init signature - and every existing caller, including
 * test_world.c - stays unchanged (world_init leaves ws->store == NULL). The
 * store is borrowed, NOT owned: the caller opened it (persist_open) and the
 * caller closes it (persist_close); world_shutdown only flushes through it. */
void world_set_persist(WorldStore *ws, PersistStore *store)
{
    if (ws == NULL)
        return;                          /* tolerate a NULL store handle too */
    ws->store = store;
}

void world_shutdown(WorldStore *ws)
{
    if (ws == NULL)
        return;

    /* Evict every resident so render slots are freed via the callback and
     * neighbour pointers are cleared. Walk the dense list from the end (evict
     * does a swap-remove, so popping the last is stable). The evict path
     * already persists each CHUNK_MODIFIED chunk (the writeback hook above), so
     * this loop IS the Section 8 shutdown flush of resident edits. */
    while (ws->resident_count > 0u) {
        Chunk *c = &ws->pool[ws->resident[ws->resident_count - 1u]];
        world_evict(ws, c->cx, c->cy, c->cz);
    }

    /* Land the region headers/indices high-water marks to disk now that every
     * resident modified chunk has been written through evict. persist_flush
     * does NOT close the handles - the store is borrowed (world_set_persist
     * comment), so the caller still owns persist_close(). NULL store -> no-op,
     * so a non-persisted world (test_world.c) shuts down exactly as in M7. */
    if (ws->store != NULL)
        (void)persist_flush(ws->store);

    free(ws->pool);
    ws->pool = NULL;
    free(ws->slabs);             /* 0.5 M1: the voxel-block sub-pool */
    ws->slabs = NULL;
}

/* ======================================================================== *
 *  Budgeted drain (Section 6)                                              *
 * ======================================================================== */

/* Pop up to WORLD_GEN_BUDGET gen requests: each brings a chunk resident
 * (world_insert -> pop slab, gen, link, queue self+neighbours for remesh).
 * Skips coords that became resident already (a duplicate enqueue, or primed). */
static void drain_gen(WorldStore *ws, int budget)
{
    int done = 0;
    while (done < budget && !gen_empty(ws)) {
        int cx = ws->genq_cx[ws->genq_head];
        int cy = ws->genq_cy[ws->genq_head];
        int cz = ws->genq_cz[ws->genq_head];
        ws->genq_head = (ws->genq_head + 1u) % WORLD_GENQ_CAP;

        if (world_get(ws, cx, cy, cz) != NULL)
            continue;                    /* already resident; not a budget spend */

        world_insert(ws, cx, cy, cz);    /* NULL only if pool full (invariant) */
        done++;
    }
}

/* Pop up to WORLD_REMESH_BUDGET remesh requests: light+mesh+upload each chunk
 * into its render slot, then clear CHUNK_DIRTY_MESH. A queued pool index whose
 * chunk was evicted (flags cleared, not dirty) is skipped without spending
 * budget, so a stale entry never touches a recycled slab. */
static void drain_remesh(WorldStore *ws, int budget)
{
    int done = 0;
    while (done < budget && !remesh_empty(ws)) {
        uint32_t pidx = ws->remeshq[ws->remeshq_head];
        ws->remeshq_head = (ws->remeshq_head + 1u) % WORLD_REMESHQ_CAP;

        Chunk *c = &ws->pool[pidx];
        if (!(c->flags & CHUNK_DIRTY_MESH))
            continue;                    /* evicted or already meshed: skip */

        if (ws->cb.mesh_upload)
            ws->cb.mesh_upload(c, ws->render_slot[pidx], ws->cb.user);

        c->flags &= ~(uint8_t)CHUNK_DIRTY_MESH;
        c->flags &= ~(uint8_t)CHUNK_GEN;
        done++;
    }
}

/* ======================================================================== *
 *  Streaming window update (the per-frame driver)                          *
 * ======================================================================== */

/* Convert a world position to a chunk coord: floor(world / CHUNK_DIM). Done in
 * integer (cast toward zero would mis-round negatives) so the window centre is
 * correct on either side of the origin. */
static int floor_div_chunk(float world)
{
    int w = (int)world;
    /* (int) truncates toward zero; for negative non-multiples that is one too
     * high, so adjust. Compare against world to catch the fractional part. */
    if ((float)w > world)
        w -= 1;                          /* world was negative & fractional */
    /* now w == floor(world); divide by CHUNK_DIM with floor semantics */
    if (w >= 0)
        return w / CHUNK_DIM;
    return -(((-w) + CHUNK_DIM - 1) / CHUNK_DIM);
}

/* Evict every resident whose (cx,cz) Chebyshev distance from the new centre
 * exceeds WORLD_RADIUS (the trailing edge). Walks the dense resident list;
 * because world_evict swap-removes, we do NOT advance i when we evict at i. */
static void evict_out_of_range(WorldStore *ws, int center_cx, int center_cy, int center_cz)
{
    uint32_t i = 0;
    while (i < ws->resident_count) {
        Chunk *c = &ws->pool[ws->resident[i]];
        int dy = c->cy - center_cy; if (dy < 0) dy = -dy;
        /* 0.5 M2: out of range horizontally OR outside the player-following band */
        if (cheby(c->cx, c->cz, center_cx, center_cz) > ws->view_radius ||
            dy > WORLD_BAND_HALF) {
            world_evict(ws, c->cx, c->cy, c->cz);
            /* a swap-remove pulled another resident into slot i; re-test it */
        } else {
            i++;
        }
    }
}

/* Enqueue for generation every in-band chunk within radius of the new centre
 * that is not already resident (the leading edge). Coords only; the actual
 * generation happens in the budgeted drain. */
static void enqueue_in_range(WorldStore *ws, int center_cx, int center_cy, int center_cz)
{
    int dx, dz, cy;

    /* Rebuild the gen queue from EMPTY. This is the dedup: the old code only
     * skipped coords that were already RESIDENT, never coords already sitting in
     * the queue, so every move re-flooded the still-pending in-band chunks
     * (drain removes only WORLD_GEN_BUDGET=8/frame). The backlog grew until the
     * ring lapped its head and silently dropped chunks (window collapse + a
     * permanent hole once the player stops). enqueue_in_range is the ONLY
     * producer and runs on every move over the WHOLE window, so clearing first
     * loses nothing: each still-needed (non-resident, in-range) coord is just
     * re-added below. The triple loop visits each coord once, so the queue holds
     * at most WORLD_WINDOW_CHUNKS (338) distinct entries < WORLD_GENQ_CAP (402):
     * no duplicates, no lap. Coords that fell OUT of range are correctly dropped
     * (we don't want to generate them). */
    ws->genq_head = ws->genq_tail = 0;

    for (cy = center_cy - WORLD_BAND_HALF; cy <= center_cy + WORLD_BAND_HALF; ++cy) {
        for (dz = -ws->view_radius; dz <= ws->view_radius; ++dz) {
            for (dx = -ws->view_radius; dx <= ws->view_radius; ++dx) {
                int cx = center_cx + dx;
                int cz = center_cz + dz;
                if (world_get(ws, cx, cy, cz) != NULL)
                    continue;            /* already resident */
                gen_enqueue(ws, cx, cy, cz);
            }
        }
    }
}

void world_stream_update(WorldStore *ws, float player_x, float player_y, float player_z)
{
    int center_cx = floor_div_chunk(player_x);
    int center_cy = floor_div_chunk(player_y);
    int center_cz = floor_div_chunk(player_z);
    int moved = (!ws->have_center ||
                 center_cx != ws->center_cx ||
                 center_cy != ws->center_cy ||
                 center_cz != ws->center_cz);

    if (moved) {
        /* trailing edge out, leading edge enqueued */
        evict_out_of_range(ws, center_cx, center_cy, center_cz);
        enqueue_in_range(ws, center_cx, center_cy, center_cz);
        ws->center_cx = center_cx;
        ws->center_cy = center_cy;
        ws->center_cz = center_cz;
        ws->have_center = 1;
    }
    /* A stationary player with no pending work falls straight through the drain
     * (both queues empty) and costs nothing. */

    drain_gen(ws, WORLD_GEN_BUDGET);
    drain_remesh(ws, WORLD_REMESH_BUDGET);
}

void world_prime(WorldStore *ws, float player_x, float player_y, float player_z)
{
    int center_cx = floor_div_chunk(player_x);
    int center_cy = floor_div_chunk(player_y);
    int center_cz = floor_div_chunk(player_z);

    /* Re-centre the window (evict out-of-range, enqueue leading edge) exactly
     * like a move, then drain to EMPTY ignoring the per-frame budget, so the
     * first frame shows the full terrain and tests are deterministic. */
    evict_out_of_range(ws, center_cx, center_cy, center_cz);
    enqueue_in_range(ws, center_cx, center_cy, center_cz);
    ws->center_cx = center_cx;
    ws->center_cy = center_cy;
    ws->center_cz = center_cz;
    ws->have_center = 1;

    /* Drain everything. Gen may push new entries onto the remesh queue, so
     * interleave until both are empty. WORLD_GENQ_CAP / WORLD_REMESHQ_CAP are
     * sized to the window, so this terminates within the window's bounds. */
    while (!gen_empty(ws) || !remesh_empty(ws)) {
        drain_gen(ws, (int)WORLD_GENQ_CAP);
        drain_remesh(ws, (int)WORLD_REMESHQ_CAP);
    }
}
