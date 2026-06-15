/* mesher.h - Greedy meshing: 16 KiB of voxels -> the canonical 12-byte vertex.
 *
 * Binding source: ARCHITECTURE.md Section 4 (which OWNS the canonical vertex
 * struct; Section 5 reads it back byte-for-byte). PER_VERTEX_BYTES = 12 and the
 * field order/offsets below are authoritative. If the mesher (writer) and the
 * renderer (reader, render.h/render.c) disagree by one byte, the world renders
 * as garbage.
 *
 * Vertex layout (matches Decision Ledger line "Per-vertex format = 12 bytes"):
 *   offset 0 : uint16 px      chunk-local X, 0..16 inclusive
 *   offset 2 : uint16 py      chunk-local Y, 0..16 inclusive
 *   offset 4 : uint16 pz      chunk-local Z, 0..16 inclusive
 *   offset 6 : uint8  mat     material id -> atlas tile select (0..255)
 *   offset 7 : uint8  face    face direction 0..5 -> normal + UV basis
 *   offset 8 : uint8  light   packed light byte: lo nibble = baked sky/block,
 *                             hi nibble = temp glow (non-normalized; shader splits)
 *   offset 9 : uint8  ao       ambient occlusion 0..255 (separate merge-key byte)
 *   offset 10: uint8  u        tile-local UV corner (tiling, 0..W)
 *   offset 11: uint8  v        tile-local UV corner (tiling, 0..H)
 *
 * light and ao are SEPARATE per-vertex bytes (independent greedy merge-key
 * fields: a shadow edge and a corner-darkening edge are independent reasons to
 * split a quad). The sun term is folded LIVE in the shader, NEVER baked, so a
 * moving sun costs zero remeshes. On upload the 4-bit nibbles expand to the
 * full byte range (nibble * 17) so the normalized-byte attribute lands on 0..1.
 */
#ifndef MESHER_H
#define MESHER_H

#include <stdint.h>
#include "chunk.h"

#define PER_VERTEX_BYTES 12

/* ---- Face directions: index into normal + UV basis tables --------------- */
typedef enum {
    FACE_NEG_X = 0,
    FACE_POS_X = 1,
    FACE_NEG_Y = 2,
    FACE_POS_Y = 3,
    FACE_NEG_Z = 4,
    FACE_POS_Z = 5
} Face;

/* The canonical on-the-wire vertex. EXACTLY 12 bytes, 4-byte aligned, every
 * attribute on a clean offset (the G70/XP driver dislikes unaligned fetches).
 * Positions are chunk-local; the shader adds u_chunk_origin. */
typedef struct {
    uint16_t px, py, pz;   /* 0 : chunk-local position, 0..16 inclusive (6 B) */
    uint8_t  mat;          /* 6 : material id -> atlas tile select       (1 B) */
    uint8_t  face;         /* 7 : Face direction 0..5 -> normal + UV basis(1 B) */
    uint8_t  light;        /* 8 : packed: lo=baked sky/block, hi=temp glow(1 B) */
    uint8_t  ao;           /* 9 : ambient occlusion                      (1 B) */
    uint8_t  u, v;         /* 10: tile-local UV corner, tiling           (2 B) */
} MeshVert;

_Static_assert(sizeof(MeshVert) == 12, "MeshVert must be exactly 12 bytes");
_Static_assert(sizeof(MeshVert) == PER_VERTEX_BYTES, "MeshVert != PER_VERTEX_BYTES");

/* CPU-side mesh output: interleaved vertices + a 16-bit index buffer.
 * Indices are GL_UNSIGNED_SHORT (Decision Ledger): a 16^3 greedy mesh never
 * approaches the 65,535-vertex ceiling, so 16-bit halves index VRAM.
 * Owned/recycled by the caller (WorldStore mesh pool in the shipping engine);
 * mesh_buffer_init grows the arrays to the given capacities. */
typedef struct {
    MeshVert *verts;
    uint16_t *indices;
    uint32_t  vert_count;
    uint32_t  index_count;
    uint32_t  vert_cap;
    uint32_t  index_cap;
} MeshBuffer;

/* ---- Lifecycle ----------------------------------------------------------- */
/* Allocate the vertex/index arrays to the requested capacities (a worst-case
 * chunk is ~64 KiB VB + 16 KiB IB; one reusable scratch buffer is the idiom).
 * Returns 0 on success, non-zero on allocation failure. */
int  mesh_buffer_init(MeshBuffer *mb, uint32_t vert_cap, uint32_t index_cap);
void mesh_buffer_free(MeshBuffer *mb);
/* Reset counts to zero without freeing storage (reuse across remeshes). */
void mesh_buffer_reset(MeshBuffer *mb);

/* ---- The greedy mesher --------------------------------------------------
 * Six sweeps per chunk (3 axes x 2 directions). Per axis, walk slice by slice;
 * each slice is a 16x16 face-mask plane; run a 2D greedy rectangle merge whose
 * merge key is { material, packed light byte (baked|glow), ao byte } - a
 * merge stops wherever ANY of those differ. (Face direction is implicit per
 * sweep, fixed for the whole plane, so it is not a key member.) Emits maximal
 * quads with tiling UVs into *out (which is reset first). Out-of-chunk
 * neighbours are treated as
 * AIR (the boundary face is therefore exposed / emitted); the chunk must be
 * re-meshed when its neighbours stream in. Returns the quad count.
 *
 * (The shipping signature takes the 6 cached neighbour pointers too; this
 * bring-up form meshes a chunk in isolation with air boundaries.) */
uint32_t greedy_mesh(const Chunk *c, MeshBuffer *out);

/* ---- The liquid greedy mesher -------------------------------------------
 * Companion to greedy_mesh for the blended liquid pass (ARCHITECTURE 5.6).
 * Meshes ONLY PHASE_LIQUID voxels, exposing faces against AIR (and against the
 * opaque/solid stream) so liquids form their own transparent VBO. Same greedy
 * merge and reset-first contract as greedy_mesh. Returns the quad count; an
 * all-solid or all-air chunk yields zero liquid quads. */
uint32_t greedy_mesh_liquid(const Chunk *c, MeshBuffer *out);

#endif /* MESHER_H */
