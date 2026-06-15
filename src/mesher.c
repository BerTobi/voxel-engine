/* mesher.c - Greedy meshing: 16 KiB of voxels -> the canonical 12-byte vertex.
 *
 * Binding source: ARCHITECTURE.md Section 4. Pure C, no GL / no OS. Six sweeps
 * per chunk (3 axes x 2 directions); per sweep, walk the 16 slices, build a
 * 16x16 face-mask plane keyed by { material, light, ao }, run the classic
 * width-then-height greedy rectangle merge, and emit one maximal quad (4
 * MeshVert + 6 uint16 indices) per merged rectangle.
 *
 * Face visibility (Section 4 / task contract): a face on voxel V toward its
 * neighbour N is emitted iff V is non-AIR and N is AIR. Visibility is now
 * NEIGHBOUR-AWARE: sample() reads across the chunk's 6 cached neigh[] pointers
 * (chunk.h, Face-enum order) when a coordinate steps out of 0..15, so a face on
 * the shared boundary plane is culled when the adjacent chunk's edge voxel is
 * solid. This is what removes the spurious dark "seam walls" between adjacent
 * solid chunks. A NULL neighbour is still treated as AIR (an isolated chunk, or
 * the world's true outer edge), so boundary faces there are exposed exactly as
 * before and isolated-chunk meshes are byte-for-byte unchanged.
 *
 * Cross-chunk AO and light BLEED remain DEFERRED (shading-only follow-up): the
 * AO ring keeps the "out-of-chunk == AIR" rule (see face_ao_level), so only the
 * GEOMETRY seam is fixed this milestone, not boundary shading.
 *
 * The vertex layout is the canonical 12-byte MeshVert (mesher.h is the source
 * of truth). Positions are chunk-local voxel coordinates in the uint16 fields,
 * 0..16 inclusive. The mat byte carries the material's atlas tile id (the shader
 * computes col=mod(a_mat,16), row=floor(a_mat/16)); u,v carry the tiling corner
 * (0..W and 0..H) so a merged WxH quad tiles correctly under GL_REPEAT.
 */
#include <stdlib.h>
#include "mesher.h"
#include "voxel.h"
#include "material.h"
#include "light.h"
#include "sim.h"   /* sim_temp_glow(): inline integer temperature->glow ramp */

/* ---- MeshBuffer lifecycle ------------------------------------------------ */

int mesh_buffer_init(MeshBuffer *mb, uint32_t vert_cap, uint32_t index_cap) {
    if (vert_cap == 0)  vert_cap = 1;
    if (index_cap == 0) index_cap = 1;
    mb->verts   = (MeshVert *)malloc((size_t)vert_cap * sizeof(MeshVert));
    mb->indices = (uint16_t *)malloc((size_t)index_cap * sizeof(uint16_t));
    if (!mb->verts || !mb->indices) {
        free(mb->verts);
        free(mb->indices);
        mb->verts = NULL;
        mb->indices = NULL;
        mb->vert_cap = mb->index_cap = 0;
        mb->vert_count = mb->index_count = 0;
        return 1;
    }
    mb->vert_cap   = vert_cap;
    mb->index_cap  = index_cap;
    mb->vert_count = 0;
    mb->index_count = 0;
    return 0;
}

void mesh_buffer_free(MeshBuffer *mb) {
    if (!mb) return;
    free(mb->verts);
    free(mb->indices);
    mb->verts = NULL;
    mb->indices = NULL;
    mb->vert_cap = mb->index_cap = 0;
    mb->vert_count = mb->index_count = 0;
}

void mesh_buffer_reset(MeshBuffer *mb) {
    mb->vert_count = 0;
    mb->index_count = 0;
}

/* Grow the vertex array to hold at least `need` more vertices. Returns 0 ok. */
static int mb_reserve_verts(MeshBuffer *mb, uint32_t need) {
    if (mb->vert_count + need <= mb->vert_cap) return 0;
    uint32_t cap = mb->vert_cap ? mb->vert_cap : 64;
    while (cap < mb->vert_count + need) cap *= 2;
    MeshVert *p = (MeshVert *)realloc(mb->verts, (size_t)cap * sizeof(MeshVert));
    if (!p) return 1;
    mb->verts = p;
    mb->vert_cap = cap;
    return 0;
}

/* Grow the index array to hold at least `need` more indices. Returns 0 ok. */
static int mb_reserve_indices(MeshBuffer *mb, uint32_t need) {
    if (mb->index_count + need <= mb->index_cap) return 0;
    uint32_t cap = mb->index_cap ? mb->index_cap : 96;
    while (cap < mb->index_count + need) cap *= 2;
    uint16_t *p = (uint16_t *)realloc(mb->indices, (size_t)cap * sizeof(uint16_t));
    if (!p) return 1;
    mb->indices = p;
    mb->index_cap = cap;
    return 0;
}

/* ---- Voxel sampling ------------------------------------------------------ */

/* A voxel sample that is neighbour-aware at chunk boundaries.
 *
 * When all of x/y/z are in-range (0..15) this is the plain in-chunk read.
 * When exactly one coordinate steps out of range (the only case the mesher
 * ever produces: every sweep step and every AO ring offset moves +-1 along a
 * single axis, so at a chunk face at most one coord is ever out by exactly 1),
 * we consult the matching cached neighbour pointer in c->neigh[dir], with the
 * canonical FACE ordering 0=-X 1=+X 2=-Y 3=+Y 4=-Z 5=+Z. The local coordinate
 * in the neighbour is recovered by wrapping +-CHUNK_DIM into 0..15.
 *
 * A NULL neighbour is treated as AIR (return an all-zero Voxel), which is
 * exactly the old "out-of-chunk == AIR" bring-up behaviour: an isolated chunk
 * (all neigh[] NULL) meshes byte-for-byte as before, so the world's true outer
 * faces still emit while interior seams between linked solid chunks cull. */
static inline Voxel sample(const Chunk *c, int x, int y, int z) {
    if (x < 0) {
        const Chunk *n = c->neigh[FACE_NEG_X];
        return n ? n->voxels[vox_index(x + CHUNK_DIM, y, z)] : 0;
    }
    if (x >= CHUNK_DIM) {
        const Chunk *n = c->neigh[FACE_POS_X];
        return n ? n->voxels[vox_index(x - CHUNK_DIM, y, z)] : 0;
    }
    if (y < 0) {
        const Chunk *n = c->neigh[FACE_NEG_Y];
        return n ? n->voxels[vox_index(x, y + CHUNK_DIM, z)] : 0;
    }
    if (y >= CHUNK_DIM) {
        const Chunk *n = c->neigh[FACE_POS_Y];
        return n ? n->voxels[vox_index(x, y - CHUNK_DIM, z)] : 0;
    }
    if (z < 0) {
        const Chunk *n = c->neigh[FACE_NEG_Z];
        return n ? n->voxels[vox_index(x, y, z + CHUNK_DIM)] : 0;
    }
    if (z >= CHUNK_DIM) {
        const Chunk *n = c->neigh[FACE_POS_Z];
        return n ? n->voxels[vox_index(x, y, z - CHUNK_DIM)] : 0;
    }
    return c->voxels[vox_index(x, y, z)];
}

/* "Empty" for face-visibility: AIR. MAT_AIR is guaranteed to be id 0, so an
 * all-zero voxel word is air. A face is emitted when the source voxel is solid
 * (non-air) and the neighbour is air. */
static inline int is_air(Voxel v) {
    return vox_mat(v) == (uint8_t)MAT_AIR;
}

/* PHASE_LIQUID classification, data-driven (MaterialDef.phase, never an id
 * switch). The mesher now emits liquids in a SEPARATE geometry stream so the
 * renderer can draw them in its alpha pass (ARCHITECTURE 5.1/5.6): greedy_mesh
 * skips PHASE_LIQUID source voxels (opaque stream only), greedy_mesh_liquid
 * emits ONLY PHASE_LIQUID source voxels (liquid stream). Air (MAT_AIR) is
 * PHASE_GAS, so this is false for air - the two passes partition the non-air
 * voxels by phase, with no voxel meshed twice and none dropped. */
static inline int is_liquid(Voxel v) {
    return material_get(vox_mat(v))->phase == (uint8_t)PHASE_LIQUID;
}

/* ---- The merge key (per potential face on a slice plane) ----------------- */

typedef struct {
    uint8_t mat;     /* atlas tile id (g_materials[id].atlas_tile)            */
    uint8_t light;   /* packed light byte: lo nibble = baked sky/block 0..15,
                        hi nibble = temp glow 0..15 (non-normalized)         */
    uint8_t ao;      /* per-face ambient occlusion byte = ao_level*17 (0..255)*/
    uint8_t present; /* 1 = a visible face here, 0 = none (or consumed)      */
} FaceKey;

static inline int key_eq(const FaceKey *a, const FaceKey *b) {
    return a->present && b->present
        && a->mat   == b->mat
        && a->light == b->light
        && a->ao    == b->ao;
}

/* Expand a 4-bit level (0..15, the voxel light field or the AO level) into the
 * full 0..255 vertex byte. *17 maps 15 -> 255 so the GL_TRUE normalized-byte
 * attribute lands cleanly on 0..1 (and 0 -> 0). render.c uploads MeshVert.light
 * and .ao as GL_UNSIGNED_BYTE GL_TRUE and does NO nibble expansion itself, so
 * the mesher must write the already-expanded byte (see light.h, "THE 4-BIT
 * CONTRADICTION, RESOLVED FOR THIS MILESTONE"). 15*17 == 255 matches the old
 * M1 full-bright placeholder exactly, so a uniformly-lit open face is byte-for-
 * byte what it was before lighting was baked. */
static inline uint8_t expand_level(uint8_t level) {
    return (uint8_t)(level * 17u);   /* 0..15 -> 0..255 (15->255) */
}

/* ---- Per-face ambient occlusion -----------------------------------------
 * One 0..15 occlusion value per EXPOSED face (the design's face-wide scalar, so
 * it fits the single `ao` merge-key byte the 12-byte vertex carries; per-corner
 * AO would need 4 bytes the format does not have - mesher.h). The face is on
 * solid voxel V toward air neighbour N (one step along axis d). Look at the 8
 * voxels that ring N in the face's own plane (perpendicular to d, at N's
 * d-coordinate): the 4 edge neighbours (+-ua, +-va around N) and the 4
 * diagonals. Count how many are SOLID (MAT_OPAQUE; out-of-chunk == not solid,
 * matching the air-boundary bring-up rule). More crowding -> more occlusion ->
 * darker. Map brightest-at-0-occluders into the 0..15 AO level:
 *     ao_level = LIGHT_MAX - (occ * LIGHT_MAX) / 8
 * (0 occ -> 15 full bright, 8 occ -> 0 max dark). occ==0 gives 15, which the
 * caller expands to 255 - identical to the old full-bright placeholder, so flat
 * open faces do not regress; only edges/corners (occ>0) darken. */
static inline int is_opaque(Voxel v) {
    return (material_get(vox_mat(v))->flags & MAT_OPAQUE) != 0;
}

/* AO ring read: out-of-chunk is AIR (an all-zero, non-opaque Voxel), NOT a
 * cross-neighbour read. This is deliberate and SEPARATE from sample()'s
 * face-visibility path. The milestone fix is GEOMETRY: face visibility reads
 * across c->neigh[] so the spurious seam walls between adjacent solid chunks
 * are culled. Cross-chunk *AO bleed* (a hot/solid neighbour darkening the
 * perpendicular boundary faces of this chunk) is a shading-only follow-up that
 * the design explicitly DEFERS alongside cross-chunk light. Routing the AO ring
 * through sample() would let a solid neighbour add occluders along the seam
 * edge, splitting an otherwise-uniform boundary face into several AO-keyed
 * quads (no visual seam, but extra geometry) and would break the per-chunk AO
 * invariant the unit tests pin (a linked solid chunk must still emit exactly 5
 * maximal faces). So the ring keeps today's "out-of-chunk == not solid"
 * boundary rule (see the AO comment above, mesher.c air-boundary note). */
static inline int ao_ring_opaque(const Chunk *c, int x, int y, int z) {
    if (x < 0 || x >= CHUNK_DIM ||
        y < 0 || y >= CHUNK_DIM ||
        z < 0 || z >= CHUNK_DIM)
        return 0; /* out-of-chunk == AIR == not an occluder (deferred AO bleed) */
    return is_opaque(c->voxels[vox_index(x, y, z)]);
}

static uint8_t face_ao_level(const Chunk *c, const int nc[3],
                             int ua, int va) {
    /* The 8 ring offsets in (ua,va) around N: 4 edges then 4 diagonals. */
    static const int ring_u[8] = { +1, -1,  0,  0, +1, +1, -1, -1 };
    static const int ring_v[8] = {  0,  0, +1, -1, +1, -1, +1, -1 };

    int occ = 0;
    for (int i = 0; i < 8; ++i) {
        int rc[3];
        rc[0] = nc[0]; rc[1] = nc[1]; rc[2] = nc[2];
        rc[ua] += ring_u[i];
        rc[va] += ring_v[i];
        if (ao_ring_opaque(c, rc[0], rc[1], rc[2]))
            ++occ;
    }
    return (uint8_t)(LIGHT_MAX - (occ * LIGHT_MAX) / 8);
}

/* ---- Quad emission ------------------------------------------------------- */

/* Emit one quad. The quad lives on a plane perpendicular to axis `d` at plane
 * coordinate `plane` (in voxel units, 0..16). The two in-plane axes are `ua`
 * and `va`; the rectangle covers [u0,u0+w) x [v0,v0+h) in those axes. `flip`
 * controls winding so the visible side faces outward (CCW front face).
 *
 * Position layout: a corner is built from (plane along d, u along ua, v along
 * va). The u,v tiling corners run 0..w and 0..h so a merged WxH quad repeats
 * the tile w-by-h times under GL_REPEAT. */
static int emit_quad(MeshBuffer *mb, int d, int ua, int va, int plane,
                     int u0, int v0, int w, int h, Face face, const FaceKey *k,
                     int flip) {
    if (mb_reserve_verts(mb, 4))   return 1;
    if (mb_reserve_indices(mb, 6)) return 1;

    /* Corner coordinates in (d, ua, va) ordered as the four rectangle corners:
     *   c0 = (u0,     v0)
     *   c1 = (u0+w,   v0)
     *   c2 = (u0+w,   v0+h)
     *   c3 = (u0,     v0+h)
     * with the constant `plane` along axis d. */
    const int cu[4] = { u0,     u0 + w, u0 + w, u0     };
    const int cv[4] = { v0,     v0,     v0 + h, v0 + h };
    /* Atlas UV corners 0..1: each face maps to exactly its ONE material tile,
     * stretched across the merged WxH quad. (Per-tile REPEAT cannot be done by
     * GL_REPEAT on a sub-tile of an atlas - the shader's v_uv=(a_uv+tile)/16
     * would sweep across neighbouring tiles for a_uv>1. True per-tile tiling
     * needs a fract() in the fragment shader and lands with the real textured
     * atlas; the placeholder tiles are flat-coloured, so stretching is invisible
     * and a single tile per face is correct.) */
    const int tu[4] = { 0, 1, 1, 0 };
    const int tv[4] = { 0, 0, 1, 1 };

    uint16_t pos[4][3];
    uint8_t  uvu[4], uvv[4];
    for (int i = 0; i < 4; ++i) {
        int p[3];
        p[d]  = plane;
        p[ua] = cu[i];
        p[va] = cv[i];
        pos[i][0] = (uint16_t)p[0];
        pos[i][1] = (uint16_t)p[1];
        pos[i][2] = (uint16_t)p[2];
        uvu[i] = (uint8_t)tu[i];
        uvv[i] = (uint8_t)tv[i];
    }

    uint32_t base = mb->vert_count;
    for (int i = 0; i < 4; ++i) {
        MeshVert *mv = &mb->verts[base + (uint32_t)i];
        mv->px = pos[i][0];
        mv->py = pos[i][1];
        mv->pz = pos[i][2];
        mv->mat   = k->mat;
        mv->face  = (uint8_t)face;
        mv->light = k->light;
        mv->ao    = k->ao;
        mv->u     = uvu[i];
        mv->v     = uvv[i];
    }
    mb->vert_count += 4;

    /* Two triangles. The corner order c0,c1,c2,c3 is CCW when viewed from the
     * +(u x v) side; `flip` reverses winding for the faces whose visible side
     * is on the -(u x v) side so the front face always points outward. */
    uint16_t *idx = &mb->indices[mb->index_count];
    if (!flip) {
        idx[0] = (uint16_t)(base + 0);
        idx[1] = (uint16_t)(base + 1);
        idx[2] = (uint16_t)(base + 2);
        idx[3] = (uint16_t)(base + 0);
        idx[4] = (uint16_t)(base + 2);
        idx[5] = (uint16_t)(base + 3);
    } else {
        idx[0] = (uint16_t)(base + 0);
        idx[1] = (uint16_t)(base + 2);
        idx[2] = (uint16_t)(base + 1);
        idx[3] = (uint16_t)(base + 0);
        idx[4] = (uint16_t)(base + 3);
        idx[5] = (uint16_t)(base + 2);
    }
    mb->index_count += 6;
    return 0;
}

/* ---- One sweep ----------------------------------------------------------
 * axis d in {0,1,2}; dir in {-1,+1} is the facing direction along +d/-d. For
 * each of the 16 voxel layers `s`, build the 16x16 face mask of faces on the
 * `dir` side of layer s, greedily merge, emit. The face plane coordinate is s
 * for dir=-1 (low side of layer s) and s+1 for dir=+1 (high side).
 *
 * `liquid_pass` selects which phase this sweep meshes and the face-visibility
 * rule, partitioning the chunk's faces into two streams (one shared merge body,
 * no duplicated merge code - ARCHITECTURE 5.1):
 *   liquid_pass == 0 (OPAQUE stream): source voxel must be non-air and NOT
 *     PHASE_LIQUID; a face is emitted toward an AIR neighbour (unchanged solid
 *     rule). A liquid neighbour is non-air, so a solid face behind a liquid
 *     stays culled exactly as before liquids were split out.
 *   liquid_pass == 1 (LIQUID stream): source voxel must be PHASE_LIQUID; a
 *     liquid face is emitted ONLY toward AIR (a liquid against solid or against
 *     another liquid culls - the pinned RENDER PLAN rule). The fill-height top
 *     surface is DEFERRED this milestone (see mesher.h note); liquids render as
 *     full translucent cubes, so the +Y face is emitted at the full cell top
 *     like any other face. */
static uint32_t sweep_axis(const Chunk *c, MeshBuffer *mb, int d, int dir,
                           Face face, int liquid_pass) {
    /* In-plane axes (ua < va keeps a stable, right-handed-ish basis). */
    int ua, va;
    if (d == 0)      { ua = 1; va = 2; }   /* X faces span (Y,Z) */
    else if (d == 1) { ua = 2; va = 0; }   /* Y faces span (Z,X) */
    else             { ua = 0; va = 1; }   /* Z faces span (X,Y) */

    /* Winding: with the c0..c3 order in emit_quad being CCW about +(ua x va),
     * a +dir face (outward normal along +d) is front-facing as-is, and a -dir
     * face needs the flip. */
    int flip = (dir < 0) ? 1 : 0;

    uint32_t quads = 0;
    FaceKey mask[CHUNK_DIM][CHUNK_DIM]; /* mask[v][u] */

    for (int s = 0; s < CHUNK_DIM; ++s) {
        int plane = (dir > 0) ? (s + 1) : s;

        /* Build the face mask for this layer. coords reconstruct (d=s, ua=u,
         * va=v); the neighbour is one step along dir on axis d. */
        for (int v = 0; v < CHUNK_DIM; ++v) {
            for (int u = 0; u < CHUNK_DIM; ++u) {
                int cc[3];
                cc[d]  = s;
                cc[ua] = u;
                cc[va] = v;
                Voxel here = sample(c, cc[0], cc[1], cc[2]);

                FaceKey *m = &mask[v][u];
                m->present = 0;
                if (is_air(here)) continue; /* air emits no faces */

                /* Phase partition: the opaque sweep meshes only non-liquid
                 * source voxels; the liquid sweep meshes only PHASE_LIQUID ones.
                 * A voxel therefore lands in exactly one stream (data-driven via
                 * MaterialDef.phase, no id switch). */
                if (liquid_pass) {
                    if (!is_liquid(here)) continue; /* opaque voxel: other pass */
                } else {
                    if (is_liquid(here)) continue;  /* liquid voxel: other pass */
                }

                int nc[3];
                nc[0] = cc[0]; nc[1] = cc[1]; nc[2] = cc[2];
                nc[d] += dir;
                Voxel neigh = sample(c, nc[0], nc[1], nc[2]);
                if (!is_air(neigh)) continue; /* occluded by solid neighbour */

                /* Visible face. Key it by appearance.
                 * Light: a face is lit by the AIR cell it faces into, so sample
                 * the neighbour's baked light (light_compute() smears surface
                 * light onto solids, so the air cell carries the level that
                 * reaches this exposed side). Then fold in the TEMPERATURE GLOW
                 * of the owning solid voxel `here` (sim_temp_glow over its temp
                 * code, 0..15): a hot voxel reads brighter than its baked light
                 * alone. Both are 0..15 levels; pack baked into the low nibble
                 * and glow into the high nibble (no max-combine; the shader splits
                 * them and ADDS the glow as emissive) (see sim.h Section 6 / the
                 * milestone viz plan). Ambient-cool voxels give glow 0, so flat
                 * cool terrain is byte-for-byte unchanged. An out-of-chunk
                 * neighbour reads 0 -> the shader's ambient floor keeps the
                 * boundary face dark-but-visible.
                 * AO: a face-wide occlusion scalar from the 8 solid voxels
                 * ringing N in the face plane. The packed light byte (baked plus
                 * glow nibbles) and ao both stay in the FaceKey, so a shadow edge,
                 * a corner-darkening edge, or a temperature step each split the
                 * greedy merge - the plume edge gets its own quad. */
                uint8_t id    = vox_mat(here);
                uint8_t glow  = sim_temp_glow(vox_temp_code(here)); /* 0..15 heat */
                uint8_t baked = vox_light(neigh);                   /* 0..15 sky  */
                m->mat     = material_get(id)->atlas_tile;
                /* Pack two 4-bit channels into the light byte (uploaded NON-
                 * normalized; the shader splits the nibbles): low = baked sky/
                 * block light (white, sun-scaled), high = temperature glow (a
                 * distinct red-orange emissive the shader ADDS). Keeping them
                 * separate is what lets heat show even where block-light already
                 * saturates near a hot source - a shared max() channel hid it. */
                m->light   = (uint8_t)((baked & 0x0Fu) | ((glow & 0x0Fu) << 4));
                m->ao      = expand_level(face_ao_level(c, nc, ua, va));
                m->present = 1;
            }
        }

        /* Greedy 2D merge: width-then-height. */
        for (int v = 0; v < CHUNK_DIM; ++v) {
            for (int u = 0; u < CHUNK_DIM; ) {
                FaceKey k = mask[v][u];
                if (!k.present) { ++u; continue; }

                /* grow width while the key matches */
                int w = 1;
                while (u + w < CHUNK_DIM && key_eq(&mask[v][u + w], &k)) ++w;

                /* grow height while the entire [u..u+w) row matches */
                int h = 1;
                while (v + h < CHUNK_DIM) {
                    int ok = 1;
                    for (int x = u; x < u + w; ++x) {
                        if (!key_eq(&mask[v + h][x], &k)) { ok = 0; break; }
                    }
                    if (!ok) break;
                    ++h;
                }

                if (emit_quad(mb, d, ua, va, plane, u, v, w, h, face, &k, flip))
                    return quads; /* allocation failure: stop, return what we have */
                ++quads;

                /* consume the merged rectangle */
                for (int yy = v; yy < v + h; ++yy)
                    for (int xx = u; xx < u + w; ++xx)
                        mask[yy][xx].present = 0;

                u += w;
            }
        }
    }
    return quads;
}

/* ---- The public mesher --------------------------------------------------- */

uint32_t greedy_mesh(const Chunk *c, MeshBuffer *out) {
    mesh_buffer_reset(out);

    uint32_t quads = 0;
    quads += sweep_axis(c, out, 0, -1, FACE_NEG_X, 0);
    quads += sweep_axis(c, out, 0, +1, FACE_POS_X, 0);
    quads += sweep_axis(c, out, 1, -1, FACE_NEG_Y, 0);
    quads += sweep_axis(c, out, 1, +1, FACE_POS_Y, 0);
    quads += sweep_axis(c, out, 2, -1, FACE_NEG_Z, 0);
    quads += sweep_axis(c, out, 2, +1, FACE_POS_Z, 0);
    return quads;
}

/* The liquid stream: same six greedy sweeps, but meshing ONLY PHASE_LIQUID
 * source voxels and emitting a face only toward AIR (sweep_axis liquid_pass=1).
 * The renderer uploads this into a second per-chunk VBO and draws it in the
 * blended liquid pass (render_end, ARCHITECTURE 5.6). An all-solid or all-air
 * chunk yields zero liquid quads, so callers that never touch liquids pay only
 * the empty-mask scan. Resets `out` first, exactly like greedy_mesh. */
uint32_t greedy_mesh_liquid(const Chunk *c, MeshBuffer *out) {
    mesh_buffer_reset(out);

    uint32_t quads = 0;
    quads += sweep_axis(c, out, 0, -1, FACE_NEG_X, 1);
    quads += sweep_axis(c, out, 0, +1, FACE_POS_X, 1);
    quads += sweep_axis(c, out, 1, -1, FACE_NEG_Y, 1);
    quads += sweep_axis(c, out, 1, +1, FACE_POS_Y, 1);
    quads += sweep_axis(c, out, 2, -1, FACE_NEG_Z, 1);
    quads += sweep_axis(c, out, 2, +1, FACE_POS_Z, 1);
    return quads;
}
