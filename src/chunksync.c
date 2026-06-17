/* chunksync.c - chunk-delta (de)serialization for 0.3 multiplayer (see chunksync.h). */
#include <stddef.h>     /* NULL */
#include "chunksync.h"
#include "worldgen.h"   /* worldgen_fill_chunk (seed regen for the diff) */
#include "chunk.h"      /* Chunk, CHUNK_VOXELS, CHUNK_DIM */
#include "voxel.h"      /* Voxel */

int chunksync_serve(int cx, int cy, int cz, unsigned char *out, int cap, void *user)
{
    ChunkSyncCtx *cs = (ChunkSyncCtx *)user;
    static Chunk cur, seedc;            /* large; single-threaded engine -> static ok */
    const Chunk *src = NULL;
    Chunk *resident;
    int idx, count = 0, pos;

    resident = world_get(cs->world, cx, cy, cz);
    if (resident != NULL)
        src = resident;                 /* current authoritative voxels */
    else if (cs->persist != NULL && persist_load_chunk(cs->persist, &cur, cx, cy, cz))
        src = &cur;                     /* evicted but persisted (host owns the save) */
    /* else src == NULL -> chunk untouched relative to the seed -> count 0 */

    if (cap < 14) return 0;
    out[0]=(unsigned char)cx;  out[1]=(unsigned char)(cx>>8);  out[2]=(unsigned char)(cx>>16);  out[3]=(unsigned char)(cx>>24);
    out[4]=(unsigned char)cy;  out[5]=(unsigned char)(cy>>8);  out[6]=(unsigned char)(cy>>16);  out[7]=(unsigned char)(cy>>24);
    out[8]=(unsigned char)cz;  out[9]=(unsigned char)(cz>>8);  out[10]=(unsigned char)(cz>>16); out[11]=(unsigned char)(cz>>24);
    pos = 14;                           /* out[12..13] = count, patched below */

    if (src != NULL) {
        worldgen_fill_chunk(&seedc, cx, cy, cz, cs->seed);
        for (idx = 0; idx < CHUNK_VOXELS; ++idx) {
            Voxel a = persist_canon(src->voxels[idx]);
            if (a != persist_canon(seedc.voxels[idx])) {
                if (pos + 6 > cap) break;            /* never overflow the buffer */
                out[pos++] = (unsigned char)idx;       out[pos++] = (unsigned char)(idx >> 8);
                out[pos++] = (unsigned char)a;         out[pos++] = (unsigned char)(a >> 8);
                out[pos++] = (unsigned char)(a >> 16); out[pos++] = (unsigned char)(a >> 24);
                ++count;
            }
        }
    }
    out[12] = (unsigned char)count; out[13] = (unsigned char)(count >> 8);
    return pos;
}

void chunksync_apply(const unsigned char *data, int len, void *user)
{
    ChunkSyncCtx *cs = (ChunkSyncCtx *)user;
    int cx, cy, cz, count, i, pos;
    if (len < 14) return;
    cx = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
    cy = (int)((uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24));
    cz = (int)((uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24));
    count = (int)(data[12] | (data[13] << 8));
    pos = 14;
    if (count == 0) return;                          /* untouched: seed copy is correct */
    if (pos + count * 6 > len) return;               /* malformed length */
    for (i = 0; i < count; ++i) {
        int idx = data[pos] | (data[pos + 1] << 8);
        uint32_t v = (uint32_t)data[pos + 2] | ((uint32_t)data[pos + 3] << 8) |
                     ((uint32_t)data[pos + 4] << 16) | ((uint32_t)data[pos + 5] << 24);
        pos += 6;
        if (idx < 0 || idx >= CHUNK_VOXELS) continue;
        /* vox_index = lx + ly*16 + lz*256 (chunk.c) -> invert to local x/y/z. A
         * not-currently-resident chunk just no-ops (re-requested on next regen). */
        world_edit_voxel(cs->world,
                         cx * CHUNK_DIM + (idx & 15),
                         cy * CHUNK_DIM + ((idx >> 4) & 15),
                         cz * CHUNK_DIM + ((idx >> 8) & 15), (Voxel)v);
    }
}
