/* light.c - The baked light-propagation pass: fills each voxel's 4-bit light.
 *
 * Pure C, no GL/OS dependency (mirrors chunk.c / mesher.c). It runs over the
 * 16 KiB voxel array of one chunk and writes the combined max(sky, block) level
 * into bits 20..23 via vox_set_light(). See light.h for the binding contract:
 * the voxel carries ONE 4-bit level (the Section-2 layout), NOT two nibbles -
 * "THE 4-BIT CONTRADICTION, RESOLVED FOR THIS MILESTONE" in light.h is the
 * authoritative note. The mesher later expands that 0..15 level to the 0..255
 * vertex byte (level * 17); this pass only owns the voxel field.
 *
 * Algorithm (per light.h, all per-chunk; flat-world is the trivial case but the
 * algorithm is the general one):
 *   1) skylight seed: each column enters the chunk top at LIGHT_MAX and passes
 *      straight down undimmed through non-opaque voxels, stopping at the first
 *      opaque one.
 *   2) skylight horizontal flood: BFS from the open-air cells, -LIGHT_FALLOFF
 *      per voxel step, so light wraps a short way under overhangs.
 *   3) blocklight flood: BFS seeded from every MAT_EMISSIVE voxel at LIGHT_MAX.
 *   4) combine + write back: vox_set_light(v, max(sky, block)); plus the
 *      surface-inherit smear (see below) so lit air bleeds onto solid top faces.
 *
 * "opaque" is material_get(vox_mat(v))->flags & MAT_OPAQUE - NOT mat==MAT_AIR.
 * Light thus passes through a future transparent-but-non-air block (glass/water)
 * correctly; for the current world the two coincide. Out-of-chunk neighbours are
 * treated as air in the horizontal flood (the bring-up air-boundary rule,
 * matching the mesher).
 *
 * CROSS-CHUNK SEAM (the faint dark line on solid top faces at a chunk edge):
 * mitigated in step 4. The surface-inherit smear is now neighbour-aware - a
 * boundary opaque voxel reads across c->neigh[] (light_seam_level) so it
 * inherits the lit open air that actually lives in the adjacent chunk instead
 * of reading 0. This is a cheap, self-contained seam reduction (it reads the
 * neighbour's OPACITY only, folding the same open-sky assumption used at the
 * chunk top, NOT the neighbour's baked light values), so it needs no relight
 * ordering and recomputes when world.c re-queues a boundary chunk on a neigh-set
 * change. A full per-voxel cross-chunk light flood (seeding all 6 edge planes
 * from cached neighbour light + relight on stream-in) remains a later-milestone
 * item, as recorded in light.h.
 */
#include "light.h"
#include "chunk.h"
#include "voxel.h"
#include "material.h"

/* Is this voxel opaque to light? Drives both the column walk and the BFS spread.
 * MAT_OPAQUE (not air-ness) is the gate, so transparent non-air blocks pass
 * light. material_get is one indexed load (material.h), inlined. */
static inline int light_opaque(Voxel v)
{
    return (material_get(vox_mat(v))->flags & MAT_OPAQUE) != 0u;
}

/* Does this voxel emit block-light? Lava / fire carry MAT_EMISSIVE. */
static inline int light_emissive(Voxel v)
{
    return (material_get(vox_mat(v))->flags & MAT_EMISSIVE) != 0u;
}

/* The 6 axis-neighbour offsets in (dx,dy,dz). The BFS spreads only along these,
 * never diagonally (light propagates face-to-face, like the CA and the mesher's
 * neighbour reads). */
static const int LIGHT_NEIGH[6][3] = {
    { +1,  0,  0 }, { -1,  0,  0 },
    {  0, +1,  0 }, {  0, -1,  0 },
    {  0,  0, +1 }, {  0,  0, -1 }
};

/* Cross-chunk seam read for the surface-inherit smear (step 4). Given a
 * neighbour coordinate that has stepped exactly one cell OUT of this chunk on a
 * single axis, return the light level that the boundary opaque voxel should be
 * allowed to inherit from across the seam. We read the cached neighbour chunk
 * (c->neigh[], same Face-enum order and CHUNK_DIM wrap as the mesher's sample()):
 *   - a NON-opaque (air/transparent) neighbour voxel contributes LIGHT_MAX, the
 *     same open-sky level a column seeds with at step 1, so a top-surface voxel
 *     at the chunk edge inherits the lit air that actually lives next door
 *     instead of reading 0 and rendering a faint dark seam line;
 *   - an OPAQUE neighbour voxel (a continuing wall) contributes 0, exactly the
 *     interior result (it would not lighten the smear either);
 *   - a NULL neighbour is the true window edge: treat it as before (no inherit).
 *
 * This is a deliberately CHEAP seam reduction, not a full cross-chunk flood: it
 * does not read the neighbour's BAKED light values (which would need a fixed
 * relight ordering on stream-in), only its opacity, and folds the same open-sky
 * assumption light_compute already makes at the chunk top. It therefore stays
 * self-contained and re-entrant, and recomputes correctly when world.c re-queues
 * a boundary chunk on a neigh-set change (the same hook the mesher's seam cull
 * rides). Full per-voxel cross-chunk light bleed remains a later-milestone item;
 * this removes the visible top-face seam at near-zero cost. */
static inline int light_seam_level(const Chunk *c, int nx, int ny, int nz)
{
    const Chunk *n;
    int li;

    if (nx < 0)              { n = c->neigh[0]; nx += CHUNK_DIM; }  /* -X */
    else if (nx >= CHUNK_DIM){ n = c->neigh[1]; nx -= CHUNK_DIM; }  /* +X */
    else if (ny < 0)         { n = c->neigh[2]; ny += CHUNK_DIM; }  /* -Y */
    else if (ny >= CHUNK_DIM){ n = c->neigh[3]; ny -= CHUNK_DIM; }  /* +Y */
    else if (nz < 0)         { n = c->neigh[4]; nz += CHUNK_DIM; }  /* -Z */
    else                     { n = c->neigh[5]; nz -= CHUNK_DIM; }  /* +Z */

    if (!n)
        return 0;                          /* true window edge: no inherit       */

    li = vox_index(nx, ny, nz);
    if (light_opaque(n->voxels[li]))
        return 0;                          /* continuing wall: dark like interior */
    return LIGHT_MAX;                      /* lit air next door bleeds onto face  */
}

/* Capacity of the BFS ring. A circular buffer of CHUNK_VOXELS + 1 slots holds
 * the whole frontier: light levels only ever rise (monotone), so a given cell
 * is at most one live entry at a time and the in-flight set never exceeds the
 * 4096 voxels of the chunk. The +1 lets head==tail mean "empty" unambiguously
 * in the ring. This keeps the queue at 16 KiB of int, not the megabyte a
 * level-multiplied bound would cost - in line with light.h's modest-scratch
 * budget for the target. */
#define LIGHT_QCAP (CHUNK_VOXELS + 1)

/* One generic flood step shared by skylight (step 2) and blocklight (step 3):
 * pop every queued cell, push light into its non-opaque axis-neighbours at
 * (level - LIGHT_FALLOFF) whenever that beats their current level. This is the
 * standard breadth-first light flood; it terminates because a cell's level only
 * ever increases and is capped at LIGHT_MAX, so each cell is enqueued a bounded
 * number of times.
 *
 *   field  : the 0..15 scratch array being flooded (sky[] or blk[])
 *   c      : the chunk (for material/opacity of neighbours)
 *   queue  : caller-owned circular ring of LIGHT_QCAP linear indices
 *   head/tail : ring indices (passed by pointer, updated in place); head==tail
 *               means empty. Both advance modulo LIGHT_QCAP. */
static void light_flood(uint8_t *field, const Chunk *c,
                        int *queue, int *head, int *tail)
{
    while (*head != *tail) {
        int idx   = queue[*head];
        *head     = (*head + 1) % LIGHT_QCAP;
        uint8_t level = field[idx];
        if (level <= LIGHT_FALLOFF)
            continue;                            /* cannot light anything past here */

        /* Recover local coords from the linear index (x-fastest, y, then z). */
        int lx = idx & 0x0F;
        int ly = (idx >> 4) & 0x0F;
        int lz = (idx >> 8) & 0x0F;

        uint8_t cand = (uint8_t)(level - LIGHT_FALLOFF);

        int n;
        for (n = 0; n < 6; ++n) {
            int nx = lx + LIGHT_NEIGH[n][0];
            int ny = ly + LIGHT_NEIGH[n][1];
            int nz = lz + LIGHT_NEIGH[n][2];

            /* Out-of-chunk == air boundary: skipped here in the horizontal
             * flood, never treated as a wall (per-chunk; the flood stays
             * self-contained and re-entrant). The visible boundary artefact -
             * dark top-face edges where the lit air is in the adjacent chunk -
             * is removed in step 4 by the neighbour-aware surface-inherit smear
             * (light_seam_level); a full per-voxel cross-chunk flood remains a
             * later-milestone item (see the file header note). */
            if (nx < 0 || nx >= CHUNK_DIM ||
                ny < 0 || ny >= CHUNK_DIM ||
                nz < 0 || nz >= CHUNK_DIM)
                continue;

            int nidx = vox_index(nx, ny, nz);
            if (light_opaque(c->voxels[nidx]))
                continue;                        /* opaque blocks the flood */

            if (cand > field[nidx]) {
                field[nidx] = cand;
                queue[*tail] = nidx;
                *tail = (*tail + 1) % LIGHT_QCAP;
            }
        }
    }
}

/* ---- The pass ------------------------------------------------------------- */

void light_compute(Chunk *c)
{
    /* Two 0..15 scratch channels + one circular index ring, all stack-local so
     * the call is self-contained and re-entrant (light.h: scratch lives outside
     * the voxel word). sky/blk are 4 KiB each; the ring is LIGHT_QCAP ints
     * (~16 KiB) - ~24 KiB total, the modest BFS scratch light.h budgets for. */
    uint8_t sky[CHUNK_VOXELS];
    uint8_t blk[CHUNK_VOXELS];
    int     queue[LIGHT_QCAP];
    int     head, tail;
    int     i, lx, ly, lz;

    /* All channels start dark; the seeds below raise them. */
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        sky[i] = 0;
        blk[i] = 0;
    }

    /* --- Step 1: skylight seed + vertical drop ----------------------------- *
     * For each (x,z) column, walk y from the top down. The chunk top is open
     * sky, so we enter at LIGHT_MAX. Skylight passes straight down undimmed
     * through non-opaque voxels; the first opaque voxel kills it (lit -> 0) for
     * the rest of the column. Everything below an opaque ceiling gets 0 sky from
     * this column; the horizontal flood (step 2) refills under overhangs. */
    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        for (lx = 0; lx < CHUNK_DIM; ++lx) {
            uint8_t lit = LIGHT_MAX;             /* open sky above the chunk */
            for (ly = CHUNK_DIM - 1; ly >= 0; --ly) {
                int idx = vox_index(lx, ly, lz);
                if (light_opaque(c->voxels[idx])) {
                    lit = 0;                     /* ceiling: shadow below */
                    /* opaque cell itself keeps sky = 0 in the array */
                } else {
                    sky[idx] = lit;              /* air carries the column light */
                }
            }
        }
    }

    /* --- Step 2: skylight horizontal flood (BFS) --------------------------- *
     * Seed with every cell already bright enough to light a neighbour (level >
     * LIGHT_FALLOFF, i.e. the open-air 15s from step 1) and flood. On flat
     * terrain (no overhangs) this raises nothing new beyond step 1. */
    head = tail = 0;
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        if (sky[i] > LIGHT_FALLOFF)
            queue[tail++] = i;
    }
    light_flood(sky, c, queue, &head, &tail);

    /* --- Step 3: blocklight flood (BFS) ------------------------------------ *
     * Every MAT_EMISSIVE voxel seeds blk[] at LIGHT_MAX and floods into non-
     * opaque neighbours. An emissive solid lights its own cell and its air
     * neighbours. */
    head = tail = 0;
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        if (light_emissive(c->voxels[i])) {
            blk[i] = LIGHT_MAX;
            queue[tail++] = i;
        }
    }
    light_flood(blk, c, queue, &head, &tail);

    /* --- Step 4: combine + write back, with the surface-inherit smear ------ *
     * Stored level = max(sky, block). But the mesher keys a face on the SOLID
     * voxel that owns it (not the air neighbour), so a flat top surface - opaque,
     * thus sky=blk=0 above - would read 0 and render black. The surface-inherit
     * rule fixes this inside light.c: an OPAQUE voxel inherits the max combined
     * level over its 6 in-chunk neighbours (a 1-cell smear), so the lit air
     * directly above an exposed solid bleeds onto its top face. Non-opaque
     * voxels just take their own max(sky, block).
     *
     * We compute every voxel's own combined level first into sky[] (reused as
     * the "combined" scratch) so the smear reads neighbours' COMBINED values,
     * not a half-updated voxel field. */
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        uint8_t s = sky[i];
        uint8_t b = blk[i];
        sky[i] = (s > b) ? s : b;                /* sky[] now holds combined */
    }

    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        for (ly = 0; ly < CHUNK_DIM; ++ly) {
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                int idx = vox_index(lx, ly, lz);
                uint8_t level = sky[idx];        /* own combined level */

                if (light_opaque(c->voxels[idx])) {
                    /* Surface-inherit: brightest neighbour bleeds onto this
                     * solid cell so its exposed faces are lit. In-chunk
                     * neighbours read their COMBINED level (sky[]); a neighbour
                     * that steps across a chunk boundary reads the seam level
                     * (light_seam_level) so a top-surface voxel at a chunk edge
                     * inherits the lit air next door instead of leaving a faint
                     * dark seam line. */
                    int n;
                    for (n = 0; n < 6; ++n) {
                        int nx = lx + LIGHT_NEIGH[n][0];
                        int ny = ly + LIGHT_NEIGH[n][1];
                        int nz = lz + LIGHT_NEIGH[n][2];
                        uint8_t nl;
                        if (nx < 0 || nx >= CHUNK_DIM ||
                            ny < 0 || ny >= CHUNK_DIM ||
                            nz < 0 || nz >= CHUNK_DIM)
                            nl = (uint8_t)light_seam_level(c, nx, ny, nz);
                        else
                            nl = sky[vox_index(nx, ny, nz)];
                        if (nl > level)
                            level = nl;
                    }
                }

                vox_set_light(&c->voxels[idx], level);
            }
        }
    }
}
