/* chunk.c - The 16 KiB voxel container: lifecycle, access, and worldgen.
 *
 * Pure C, no GL/OS dependency (ARCHITECTURE.md Section 2.4). Coordinate math
 * is the shift/mask vox_index from chunk.h; the voxel word is touched only
 * through the masked accessors in voxel.h (never C bitfields). These are the
 * simple bring-up entry points; the shipping engine sources chunks from the
 * WorldStore slab pool.
 */
#include <stdlib.h>

#include "chunk.h"
#include "voxel.h"
#include "material.h"

/* Ambient temperature every fresh voxel starts at: ~20 C -> code 60.
 * Encoded through the binding codec so the worked anchor (20 C -> code 60,
 * Section 2.2) holds rather than being hand-baked here. */
#define CHUNK_AMBIENT_C 20.0

/* How many voxel rows of dirt cap a flat-world column, just under the
 * surface. Stone fills everything below that band. */
#define FLAT_DIRT_DEPTH 4

/* ---- Lifecycle ----------------------------------------------------------- */

/* calloc gives us an all-zero Chunk: every voxel is MAT_AIR (id 0) with temp
 * code 0 (-40 C) and clear flags - a valid, fully-air chunk per material.h's
 * MAT_AIR==0 contract. Worldgen overwrites material and temperature at once. */
Chunk *chunk_alloc(int cx, int cy, int cz)
{
    int d;
    Chunk *c = calloc(1, sizeof(Chunk));
    if (c == NULL)
        return NULL;
    /* 0.5 M1: voxels is no longer inline. The standalone/bring-up path OWNS its
     * 16 KiB block (calloc -> all MAT_AIR), marked slab_idx == -1 so chunk_free
     * releases it (WorldStore pool chunks borrow from the slab sub-pool instead
     * and are never chunk_free'd). */
    c->voxels = calloc(CHUNK_VOXELS, sizeof(Voxel));
    if (c->voxels == NULL) { free(c); return NULL; }
    c->slab_idx = -1;
    c->uniform_word = 0;
    c->cx = cx;
    c->cy = cy;
    c->cz = cz;
    /* calloc already zeroed neigh[], but be explicit: NULL == no neighbour,
     * which the mesher's sample() treats as AIR at the chunk boundary. */
    for (d = 0; d < 6; ++d)
        c->neigh[d] = NULL;
    return c;
}

void chunk_free(Chunk *c)
{
    if (c == NULL)
        return;
    if (c->slab_idx < 0)        /* standalone-owned block (pool blocks are freed by world) */
        free(c->voxels);
    free(c);
}

/* ---- Neighbour linking --------------------------------------------------- *
 * Wire every chunk's neigh[6] across a static grid of (nx*ny*nz) chunks. grid[]
 * is the flat array indexed i = gx + gy*nx + gz*nx*ny (the worldgen loop's
 * order). For each chunk we point neigh[] at the 6 axis-adjacent grid cells in
 * the canonical Face order (0=-X,1=+X,2=-Y,3=+Y,4=-Z,5=+Z); a grid edge (or a
 * NULL grid slot) leaves that neighbour NULL, so the world's outer faces still
 * emit exactly like an isolated chunk. Run AFTER all chunks are generated and
 * BEFORE meshing - the mesher reads a neighbour's voxels at the seam, so they
 * must already exist. No GL deps, so this is unit-testable on its own. */
void chunk_link_neighbours(Chunk **grid, int nx, int ny, int nz)
{
    int gx, gy, gz;

    if (grid == NULL)
        return;

    for (gz = 0; gz < nz; ++gz) {
        for (gy = 0; gy < ny; ++gy) {
            for (gx = 0; gx < nx; ++gx) {
                Chunk *c = grid[gx + gy * nx + gz * nx * ny];
                if (c == NULL)
                    continue;

                c->neigh[0] = (gx > 0)
                    ? grid[(gx - 1) + gy * nx + gz * nx * ny] : NULL; /* -X */
                c->neigh[1] = (gx < nx - 1)
                    ? grid[(gx + 1) + gy * nx + gz * nx * ny] : NULL; /* +X */
                c->neigh[2] = (gy > 0)
                    ? grid[gx + (gy - 1) * nx + gz * nx * ny] : NULL; /* -Y */
                c->neigh[3] = (gy < ny - 1)
                    ? grid[gx + (gy + 1) * nx + gz * nx * ny] : NULL; /* +Y */
                c->neigh[4] = (gz > 0)
                    ? grid[gx + gy * nx + (gz - 1) * nx * ny] : NULL; /* -Z */
                c->neigh[5] = (gz < nz - 1)
                    ? grid[gx + gy * nx + (gz + 1) * nx * ny] : NULL; /* +Z */
            }
        }
    }
}

/* ---- Voxel access (bounds-checked convenience over vox_index) ------------ */

/* Out-of-range reads return all-air (0) rather than indexing past the array;
 * the hot path uses vox_index directly, this is the safe convenience form. */
Voxel chunk_get(const Chunk *c, int lx, int ly, int lz)
{
    if (lx < 0 || lx >= CHUNK_DIM ||
        ly < 0 || ly >= CHUNK_DIM ||
        lz < 0 || lz >= CHUNK_DIM)
        return (Voxel)0;
    return chunk_vox(c, vox_index(lx, ly, lz));   /* 0.5 M1: uniform-aware */
}

void chunk_set(Chunk *c, int lx, int ly, int lz, Voxel v)
{
    if (lx < 0 || lx >= CHUNK_DIM ||
        ly < 0 || ly >= CHUNK_DIM ||
        lz < 0 || lz >= CHUNK_DIM)
        return;
    c->voxels[vox_index(lx, ly, lz)] = v;
    c->flags |= CHUNK_DIRTY_MESH;   /* edited content -> must remesh */
}

/* ---- Whole-chunk fill ---------------------------------------------------- */

/* 0.5 M1: bring-up/test chunks are often `Chunk c; memset(&c,0,sizeof c);` which
 * leaves voxels == NULL. chunk_fill / chunk_gen_flat are TEST/bring-up-only (the
 * shipping engine fills via worldgen into a slab-backed chunk), so they lazily
 * allocate a standalone block (slab_idx == -1) when handed an unbacked chunk. */
static void chunk_ensure_block(Chunk *c)
{
    if (c->voxels == NULL) {
        c->voxels   = calloc(CHUNK_VOXELS, sizeof(Voxel));
        c->slab_idx = -1;
    }
}

void chunk_fill(Chunk *c, Voxel v)
{
    int i;
    chunk_ensure_block(c);
    for (i = 0; i < CHUNK_VOXELS; ++i)
        c->voxels[i] = v;
    c->flags |= CHUNK_DIRTY_MESH;
}

/* ---- Worldgen stub: flat world ------------------------------------------- *
 * world-Y below ground_height is solid: stone, capped by FLAT_DIRT_DEPTH rows
 * of dirt just under the surface; world-Y at/above ground_height is air. The
 * chunk's own cy places its 16 rows within world space (worldY = cy*16 + ly).
 * Every voxel is stamped to ambient temperature (code 60); solids hold fill=15
 * per the SOLID/POWDER rule in Section 2.1 (air keeps fill 0). */
void chunk_gen_flat(Chunk *c, int ground_height)
{
    uint8_t ambient = temp_encode_c(CHUNK_AMBIENT_C);
    int lx, ly, lz;

    chunk_ensure_block(c);   /* 0.5 M1: test/bring-up chunk may be unbacked */

    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        for (ly = 0; ly < CHUNK_DIM; ++ly) {
            int world_y = c->cy * CHUNK_DIM + ly;
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                Voxel v = 0;

                if (world_y >= ground_height) {
                    /* air column above the surface */
                    vox_set_mat(&v, MAT_AIR);
                } else if (world_y >= ground_height - FLAT_DIRT_DEPTH) {
                    /* dirt layer near the top */
                    vox_set_mat(&v, MAT_DIRT);
                    vox_set_fill(&v, 15);
                } else {
                    /* stone bulk below */
                    vox_set_mat(&v, MAT_STONE);
                    vox_set_fill(&v, 15);
                }

                vox_set_temp_code(&v, ambient);
                c->voxels[vox_index(lx, ly, lz)] = v;
            }
        }
    }

    c->flags |= CHUNK_DIRTY_MESH | CHUNK_GEN;
}
