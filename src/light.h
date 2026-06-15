/* light.h - The baked light-propagation pass: fills each voxel's 4-bit light.
 *
 * Binding source: ARCHITECTURE.md Decision Ledger ("Lighting" row, line 103) and
 * Section 2 (voxel layout: bits 20..23 = baked light = max(sky,block) 0..15) and
 * Section 4 (the mesher reads vox_light/vox_ao in the cache line it already holds
 * for the material test, and folds them into the canonical 12-byte vertex).
 *
 * WHAT THIS PASS OWNS, AND WHAT IT DOES NOT
 * -----------------------------------------
 * This pass owns the voxel's `baked light` field (bits 20..23) ONLY. It writes
 * each voxel a single 4-bit level 0..15 via vox_set_light(). It is a *mesh-time
 * input*: run it on a chunk whenever the chunk goes dirty, BEFORE greedy_mesh()
 * reads vox_light() to key faces. It never touches material/temp/fill/flags, and
 * the simulation never reads what it writes (Section 2: light/ao are the only
 * fields the CA may treat as don't-care). Ambient occlusion is NOT computed here
 * - AO is a face-relative corner term derived in the mesher from the solid
 * neighbourhood of each emitted face, not a per-voxel scalar (see mesher.c).
 *
 * THE 4-BIT CONTRADICTION, RESOLVED FOR THIS MILESTONE
 * ----------------------------------------------------
 * ARCHITECTURE Section 2 (binding voxel layout) gives the voxel ONE 4-bit light
 * field defined as max(sky, block). ARCHITECTURE Section 5.1 (the vertex-format
 * prose) describes the per-vertex `light` byte as carrying SEPARATE 4-bit sky |
 * 4-bit block nibbles. Both cannot be true: the voxel word only has 4 light bits
 * and is full (the only free bit is VF_RESERVED, and Section 2 forbids claiming a
 * new persistent per-voxel field without a Section-2 edit that displaces one).
 *
 * DECISION for the lighting milestone (binding here, recorded as a doc-recon
 * open question in the milestone notes): the BINDING voxel layout wins. The
 * voxel stores ONE combined level = max(skylight, blocklight) in 0..15. The
 * mesher expands that single 0..15 into the 0..255 vertex `light` byte (level *
 * 17, so 15 -> 255 lands cleanly on the GL_TRUE normalized-byte attribute). The
 * shader sun-scales that one channel and adds an ambient floor (see render.c).
 * Storing sky and block SEPARATELY (the Section 5.1 two-nibble vertex) is
 * DEFERRED: it needs more than the 4 voxel light bits available and would force
 * a Section-2 layout edit. Until then the single combined channel is the
 * contract, and the vertex `light` byte carries that one value - not two
 * nibbles. Section 5.1's two-nibble wording is the thing to reconcile, not this.
 *
 * SEAMS (cross-chunk): DEFERRED to a later milestone, matching the rest of the
 * bring-up engine. light_compute() is PER-CHUNK and assumes open sky above the
 * chunk: every voxel column enters the chunk's top face already at full skylight
 * (LIGHT_MAX), and out-of-chunk neighbours are treated as air for horizontal
 * spread (so a chunk edge does not wrongly clamp light to 0). This mirrors the
 * mesher's existing "out-of-chunk neighbour == AIR" bring-up rule (mesher.c).
 * The shipping pass will seed the top plane and the 4 side planes from the 6
 * cached neighbour edge planes and re-light on neighbour stream-in; that is the
 * same deferral the mesher already carries, recorded once here.
 */
#ifndef LIGHT_H
#define LIGHT_H

#include "chunk.h"

/* The 4-bit light field is 0..15. Full skylight / a block-light source both sit
 * at LIGHT_MAX; horizontal/transmission spread attenuates by LIGHT_FALLOFF per
 * voxel step (so light reaches 15 cells before hitting 0). These mirror the 4
 * bits of vox_light(); do not raise LIGHT_MAX past 15 (it would clip the field).*/
#define LIGHT_MAX      15   /* full skylight and emissive source level          */
#define LIGHT_FALLOFF   1   /* level lost per voxel step in the BFS flood        */

/* Baked light pass for one chunk. Fills every voxel's 4-bit light field with
 * combined max(skylight, blocklight):
 *
 *   - SKYLIGHT: each column enters from the chunk top at LIGHT_MAX, passes
 *     straight down through non-opaque (MAT_OPAQUE-clear) voxels undimmed, and
 *     stops at the first opaque voxel. Open-air voxels then flood horizontally,
 *     losing LIGHT_FALLOFF per step (a BFS), so light wraps a short way under
 *     overhangs. (For the current flat world this is just "air = 15, solid = 0".)
 *
 *   - BLOCKLIGHT: every MAT_EMISSIVE voxel seeds a BFS at LIGHT_MAX, spreading
 *     into non-opaque neighbours and losing LIGHT_FALLOFF per step.
 *
 *   - COMBINED: each voxel's stored level is max(sky, block), written once via
 *     vox_set_light(). Opaque voxels keep whatever level reaches their own cell
 *     (so a lit solid face reads a sensible level when the mesher samples the
 *     SOLID side); air with nothing reaching it lands at 0 and the shader's
 *     ambient floor keeps it dark-but-visible rather than pure black.
 *
 * Idempotent: safe to re-run on a chunk (it recomputes from material state, not
 * from the previous light values). Call it after worldgen / after any edit that
 * sets CHUNK_DIRTY_MESH and BEFORE greedy_mesh() for that chunk. No allocation,
 * no GL, no OS - pure C over the 16 KiB voxel array, like the mesher.
 *
 * Scratch: the BFS needs a visited/queue buffer; light_compute() owns a single
 * stack-local CHUNK_VOXELS-sized scratch array (4 KiB of bytes) internally, so
 * the call is self-contained and re-entrant. (Section 2: transient per-voxel
 * scratch lives outside the voxel word, never in it.) */
void light_compute(Chunk *c);

#endif /* LIGHT_H */
