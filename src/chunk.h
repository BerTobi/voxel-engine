/* chunk.h - The 16 KiB voxel container and its hot-path metadata.
 *
 * Binding source: ARCHITECTURE.md Section 2.4. A chunk is 16x16x16 = 4096
 * voxels = exactly 16 KiB of voxel data - half the 32 KB L1D. It is the unit
 * of meshing, streaming, and storage, but NOT the unit of simulation wake-up
 * (the active-voxel front is a sub-chunk index list the CA owns; the chunk
 * merely hosts an active-count hint).
 *
 * Interior layout is x-fastest, then y, then z (z-outer), so a row of 16
 * x-neighbours is contiguous - matching the mesher's slice walk and the CA's
 * +x/-x neighbour reads. Coordinate math is pure shift/mask (16 divides the
 * world cleanly), no divides on the hot path.
 *
 * NOTE: This is the engine-facing Chunk used by the mesher and worldgen stubs.
 * The full WorldStore-owned Chunk (Section 1.5/2.4) additionally carries the
 * cached neigh[6] pointers, the CA active list, and the persistence flags;
 * those fields are owned by their respective subsystems and added there. The
 * neigh[6] pointers are now present here so the mesher can read across chunk
 * boundaries and cull seam faces; the CA active list / persistence flags remain
 * deferred to their owning subsystems.
 */
#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include "voxel.h"

/* ---- Chunk-level flags --------------------------------------------------- */
#define CHUNK_DIRTY_MESH 0x01u  /* authoritative remesh trigger (Section 4)        */
#define CHUNK_MODIFIED   0x02u  /* player-altered: must persist on eviction        */
#define CHUNK_GEN        0x04u  /* freshly generated, not yet meshed               */
#define CHUNK_MODIFIED_BY_SIM 0x08u /* 0.4: the CA mutated this chunk (host streams it) */
#define CHUNK_UNIFORM    0x10u  /* 0.5 M1: voxels==NULL, content is uniform_word (air)  */

typedef struct Chunk {
    /* 0.5 M1 sparse-air: the 16 KiB voxel block is no longer inline. A NON-uniform
     * chunk borrows a block from the WorldStore slab sub-pool (voxels != NULL,
     * slab_idx >= 0); a UNIFORM chunk (72% of a resident window are pure air) holds
     * voxels == NULL + CHUNK_UNIFORM and its single repeated value in uniform_word,
     * borrowing no block. READ a voxel through chunk_vox() (handles both). A
     * standalone chunk_alloc() chunk owns a malloc'd block (slab_idx == -1, voxels
     * != NULL) for the bring-up/test path. */
    Voxel   *voxels;                /* 16 KiB block, or NULL if CHUNK_UNIFORM        */
    Voxel    uniform_word;          /* the repeated value when voxels == NULL        */
    int32_t  slab_idx;             /* WorldStore slab index, or -1 (standalone/none) */
    int      cx, cy, cz;            /* this chunk's coords (also recoverable from key) */
    /* Cached axis-adjacent neighbour pointers (Section 2.4). The mesher reads
     * the 6 neighbour edge planes through these so a boundary face is culled
     * when the adjacent chunk's voxel is solid. Index order matches the Face
     * enum (mesher.h) exactly:
     *   neigh[0] = -X (FACE_NEG_X)   neigh[1] = +X (FACE_POS_X)
     *   neigh[2] = -Y (FACE_NEG_Y)   neigh[3] = +Y (FACE_POS_Y)
     *   neigh[4] = -Z (FACE_NEG_Z)   neigh[5] = +Z (FACE_POS_Z)
     * A NULL entry means "no neighbour" -> the mesher treats that boundary as
     * AIR (face emitted), preserving the isolated-chunk behaviour. calloc and
     * memset(&c,0,sizeof c) leave these all-NULL, so no allocator/test change. */
    struct Chunk *neigh[6];         /* 6 cached neighbour pointers, Face-enum order */
    uint16_t active_count;          /* sub-chunk activity hint for the sim         */
    uint8_t  flags;                 /* CHUNK_DIRTY_MESH | CHUNK_MODIFIED | CHUNK_GEN */
    uint8_t  _pad;
} Chunk;

/* Linear index inside a chunk: x-fastest, y, then z. lx+ly*16+lz*256.
 * Inputs are local 0..15; no bounds check (hot path). */
static inline int vox_index(int lx, int ly, int lz) {
    return lx + (ly << 4) + (lz << 8);     /* lx + ly*16 + lz*256 */
}

/* 0.5 M1: read a voxel by linear index, handling sparse-air. A realized chunk
 * (voxels != NULL) reads its block directly; a UNIFORM chunk returns its single
 * repeated word. Use this at any site that may see a NEIGHBOUR chunk (which can
 * be uniform-air) - the mesher/light seam reads, the CA cross-chunk reads, and
 * persist/net encode. Hot loops over a chunk's OWN content operate on realized
 * chunks (uniform chunks early-out of meshing/lighting/sim) and may index
 * c->voxels[] directly. No bounds check (hot path; caller passes 0..4095). */
static inline Voxel chunk_vox(const struct Chunk *c, int idx) {
    return c->voxels ? c->voxels[idx] : c->uniform_word;
}

/* ---- Lifecycle ----------------------------------------------------------- */
/* Allocate / free a Chunk. In the shipping engine chunks come from the
 * WorldStore slab pool; these are the simple bring-up entry points. */
Chunk *chunk_alloc(int cx, int cy, int cz);
void   chunk_free(Chunk *c);

/* ---- Voxel access (bounds-checked convenience over vox_index) ------------ */
Voxel  chunk_get(const Chunk *c, int lx, int ly, int lz);
void   chunk_set(Chunk *c, int lx, int ly, int lz, Voxel v);

/* Fill the whole chunk with one voxel value (e.g. all-air, all-stone). */
void   chunk_fill(Chunk *c, Voxel v);

/* Worldgen stub: a flat world. Voxels at world-Y below ground_height are
 * solid (stone), at/above are air. ground_height is in voxels, measured in
 * world space; the chunk's own cy positions its 16 rows within that. */
void   chunk_gen_flat(Chunk *c, int ground_height);

/* ---- Neighbour linking --------------------------------------------------- */
/* Wire every chunk's neigh[6] pointers across a static grid of chunks. grid[]
 * is the flat array of (nx*ny*nz) chunk pointers indexed
 *   i = gx + gy*nx + gz*nx*ny
 * (the worldgen loop's order). For each chunk this sets neigh[] to the 6
 * axis-adjacent grid cells, leaving NULL at the grid edge so the world's outer
 * faces still emit (exactly like an isolated chunk). Must run AFTER all chunks
 * are allocated/generated and BEFORE meshing. Lives here (no GL deps) so the
 * link logic is unit-testable. NULL grid slots are tolerated. */
void   chunk_link_neighbours(Chunk **grid, int nx, int ny, int nz);

#endif /* CHUNK_H */
