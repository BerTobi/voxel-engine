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
 *   i32 cx | i32 cy | i32 cz | u16 nruns | nruns * (u16 start, u16 len, u32 voxel)
 * nruns 0 == "untouched, the seed copy is already correct".
 *
 * 0.5 (proto 4): the delta is RLE'd. Each record covers a run of `len` index-
 * CONSECUTIVE voxels [start, start+len) that all carry the same canon word. A
 * water flood (a chunk of uniform same-temp water) collapses to a single run;
 * scattered heat edits stay one run each. The applier expands runs voxel-by-voxel,
 * so per-voxel apply cost is unchanged - the win is purely on the wire (a flooded
 * 4096-voxel chunk: 24 KB -> 22 B). The serializer never fragments; NET_CHUNK_MAX
 * is sized for the worst case (4096 single-voxel runs) so it cannot truncate.
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
