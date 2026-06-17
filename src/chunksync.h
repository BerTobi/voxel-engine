/* chunksync.h - 0.3 multiplayer chunk-delta (de)serialization.
 *
 * Separates the WORLD-COUPLED chunk-sync logic from net.c (pure byte transport)
 * and main.c (wiring), so the serialize/apply round-trip is unit-testable headless
 * (test_chunksync.c) without GL or sockets.
 *
 * The base terrain is regenerated from the shared seed on every machine; only the
 * EDITS (the delta from the seed) are sent. The host serializes the voxels of a
 * chunk that differ from its seed value; the client applies them onto its own
 * seed-regenerated chunk. Wire payload (little-endian, the MSG_CDATA body):
 *   i32 cx | i32 cy | i32 cz | u16 count | count * (u16 local-index, u32 voxel)
 * count 0 == "untouched, the seed copy is already correct".
 */
#ifndef CHUNKSYNC_H
#define CHUNKSYNC_H

#include <stdint.h>
#include "world.h"
#include "persist.h"

/* Context threaded into the net chunk callbacks. */
typedef struct {
    WorldStore   *world;
    PersistStore *persist;   /* host only (authoritative save); may be NULL */
    uint64_t      seed;
} ChunkSyncCtx;

/* HOST serializer (matches net_set_chunk_server's callback signature). Writes
 * chunk (cx,cy,cz)'s delta-from-seed into out[cap] and returns the byte length
 * (>= 14, the header). Source of truth: the resident chunk, else the persisted
 * chunk, else "untouched" (count 0). user is a ChunkSyncCtx*. */
int  chunksync_serve(int cx, int cy, int cz, unsigned char *out, int cap, void *user);

/* CLIENT applier (matches net_set_chunk_apply's callback signature). Patches the
 * local (seed-regenerated) chunk with the host's delta via world_edit_voxel - a
 * no-op for voxels in a chunk that is not resident. user is a ChunkSyncCtx*. */
void chunksync_apply(const unsigned char *data, int len, void *user);

#endif /* CHUNKSYNC_H */
