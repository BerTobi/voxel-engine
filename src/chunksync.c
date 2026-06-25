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
    /* 0.5 M1: voxels is no longer inline - give these scratch chunks static backing
     * blocks (slab_idx = -1 marks them standalone-owned, never freed). */
    static Voxel cur_blk[CHUNK_VOXELS], seed_blk[CHUNK_VOXELS];
    const Chunk *src = NULL;
    Chunk *resident;
    int idx, count = 0, pos;

    cur.voxels = cur_blk;     cur.slab_idx   = -1;
    seedc.voxels = seed_blk;  seedc.slab_idx = -1;

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
    pos = 14;                           /* out[12..13] = nruns, patched below */

    /* 0.5: RLE the delta-from-seed. A voxel is "changed" if its canon word differs
     * from the seed's; we coalesce a maximal run of index-CONSECUTIVE changed voxels
     * that all carry the SAME canon word into one (start,len,voxel) record. Water
     * floods (a chunk of same-temp water) collapse to a single run; scattered heat
     * edits stay one run each. Iteration is in vox_index order so a run is just a
     * contiguous index range, which the applier expands trivially. */
    if (src != NULL) {
        worldgen_fill_chunk(&seedc, cx, cy, cz, cs->seed);
        idx = 0;
        while (idx < CHUNK_VOXELS) {
            Voxel a = persist_canon(chunk_vox(src, idx));   /* src may be uniform-air */
            int start, run;
            if (a == persist_canon(seedc.voxels[idx])) { ++idx; continue; }  /* unchanged */
            start = idx; run = 1; ++idx;
            while (idx < CHUNK_VOXELS) {                     /* extend the run */
                Voxel b = persist_canon(chunk_vox(src, idx));
                if (b != a || b == persist_canon(seedc.voxels[idx])) break;
                ++run; ++idx;
            }
            if (pos + 8 > cap) break;            /* never overflow (buffer sized for worst case) */
            out[pos++] = (unsigned char)start;     out[pos++] = (unsigned char)(start >> 8);
            out[pos++] = (unsigned char)run;       out[pos++] = (unsigned char)(run >> 8);
            out[pos++] = (unsigned char)a;         out[pos++] = (unsigned char)(a >> 8);
            out[pos++] = (unsigned char)(a >> 16); out[pos++] = (unsigned char)(a >> 24);
            ++count;
        }
    }
    out[12] = (unsigned char)count; out[13] = (unsigned char)(count >> 8);  /* nruns */
    return pos;
}

void chunksync_apply(const unsigned char *data, int len, void *user)
{
    ChunkSyncCtx *cs = (ChunkSyncCtx *)user;
    int cx, cy, cz, nruns, r, pos;
    Chunk *c;
    if (len < 14) return;
    cx = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
    cy = (int)((uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24));
    cz = (int)((uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24));
    nruns = (int)(data[12] | (data[13] << 8));
    pos = 14;
    if (nruns > 0 && pos + nruns * 8 > len) return;  /* malformed length */

    /* 0.5: the delta is the FULL delta-from-seed. Reset the resident chunk to seed
     * BEFORE applying so cells the host reverted to seed-equal (and therefore omitted
     * from this delta) stop lingering on the client - the additive-apply desync that
     * left a trail of phantom water behind a moving body. Skip a uniform-air chunk
     * with an empty delta (it already equals its seed word: nothing to clear, and no
     * slab to spend realizing it). A non-resident chunk is re-requested on regen. */
    c = world_get(cs->world, cx, cy, cz);
    if (c == NULL) return;
    if (c->voxels != NULL || nruns > 0)
        world_reset_to_seed(cs->world, cx, cy, cz);
    if (nruns == 0) return;                          /* now exactly seed: done */

    for (r = 0; r < nruns; ++r) {                    /* 0.5: expand each RLE run */
        int start = data[pos] | (data[pos + 1] << 8);
        int run   = data[pos + 2] | (data[pos + 3] << 8);
        uint32_t v = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8) |
                     ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
        int k;
        pos += 8;
        for (k = 0; k < run; ++k) {
            int idx = start + k;
            if (idx < 0 || idx >= CHUNK_VOXELS) continue;
            /* vox_index = lx + ly*16 + lz*256 (chunk.c) -> invert to local x/y/z. A
             * not-currently-resident chunk just no-ops (re-requested on next regen). */
            world_edit_voxel(cs->world,
                             cx * CHUNK_DIM + (idx & 15),
                             cy * CHUNK_DIM + ((idx >> 4) & 15),
                             cz * CHUNK_DIM + ((idx >> 8) & 15), (Voxel)v);
        }
    }
}
