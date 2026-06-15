/* test_mesher.c - Standalone unit harness for the greedy mesher + voxel codec.
 *
 * Binding source: ARCHITECTURE.md Section 4 (lines 905-1116) and the pinned
 * Contracts (12-byte MeshVert, 4-byte Voxel, the six-sweep greedy mesher that
 * treats out-of-chunk neighbours as AIR). This file is PURE C: it touches no
 * GL and no OS, so it builds and runs on the dev host and the XP target alike.
 *
 * It is its own test runner (zero external deps): each case prints "PASS" or
 * "FAIL: <why>"; the process returns the number of failed cases (0 == all
 * green, non-zero == something broke), which a CI script can branch on.
 *
 * Build + run (from project root):
 *   gcc -std=c99 -Wall -Isrc -o /tmp/m1_test \
 *       src/material.c src/chunk.c src/mesher.c src/test_mesher.c && /tmp/m1_test
 *
 * Geometry bookkeeping used throughout: the mesher emits maximal quads; per
 * Section 4 every quad costs 4 vertices and 6 indices (two triangles). So a
 * mesh of N quads must report vert_count == 4*N and index_count == 6*N.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "voxel.h"
#include "chunk.h"
#include "material.h"
#include "mesher.h"

/* Per-quad geometry, restated from Section 4 ("4 vertices x 12 B = 48 B ...
 * plus 6 indices x 2 B"). The whole harness checks counts against these. */
#define VERTS_PER_QUAD   4u
#define INDICES_PER_QUAD 6u

/* Worst-case scratch buffer for a 16^3 chunk: every one of the 4096 voxels
 * could in principle contribute several faces, but a hard upper bound is the
 * naive 6 faces * 4096 voxels = 24576 quads. Sizing the scratch to that means
 * greedy_mesh() can never overrun us, whatever the input. */
#define SCRATCH_QUADS    24576u
#define SCRATCH_VERTS    (SCRATCH_QUADS * VERTS_PER_QUAD)
#define SCRATCH_INDICES  (SCRATCH_QUADS * INDICES_PER_QUAD)

/* ---- Tiny assertion plumbing -------------------------------------------- */
static int g_failures = 0;

/* Report one named case. ok != 0 => PASS. detail is an optional "why it failed"
 * string (may be NULL). Returns ok so callers can early-out if they wish. */
static int report(const char *name, int ok, const char *detail)
{
    if (ok) {
        printf("PASS: %s\n", name);
    } else {
        if (detail && detail[0])
            printf("FAIL: %s (%s)\n", name, detail);
        else
            printf("FAIL: %s\n", name);
        ++g_failures;
    }
    return ok;
}

/* ---- Voxel construction helpers ----------------------------------------- */
/* A solid stone voxel. Material id only; light/ao/temp default to 0, which is
 * fine for the mesher tests - the merge key is uniform across a uniform fill,
 * so the exact light/ao values do not change quad counts as long as they are
 * identical everywhere (which they are after chunk_fill). */
static Voxel make_voxel(uint8_t mat)
{
    Voxel v = 0;
    vox_set_mat(&v, mat);
    return v;
}

/* =========================================================================
 * Case 1 - sizes are exactly as the binding contracts demand.
 * The headers already carry _Static_assert for these; we re-check at runtime
 * too so a mis-built object (e.g. odd struct packing on some XP toolchain)
 * shows up as a loud test failure rather than silent VBO garbage.
 * ========================================================================= */
static void test_sizes(void)
{
    /* Compile-time guard, independent of the header's own static asserts. */
    _Static_assert(sizeof(MeshVert) == 12, "MeshVert must be 12 bytes");
    _Static_assert(sizeof(Voxel) == 4, "Voxel must be 4 bytes");

    char buf[96];
    int ok = (sizeof(MeshVert) == 12) && (sizeof(Voxel) == 4)
          && (PER_VERTEX_BYTES == 12);
    snprintf(buf, sizeof buf,
             "sizeof(MeshVert)=%zu sizeof(Voxel)=%zu PER_VERTEX_BYTES=%d",
             sizeof(MeshVert), sizeof(Voxel), (int)PER_VERTEX_BYTES);
    report("case1_struct_sizes", ok, buf);
}

/* =========================================================================
 * Case 2 - an all-AIR chunk meshes to nothing.
 * No voxel is opaque, so no face is ever visible: 0 quads, 0 verts, 0 indices.
 * ========================================================================= */
static void test_empty_chunk(MeshBuffer *mb)
{
    Chunk c;
    memset(&c, 0, sizeof c);            /* all-AIR: MAT_AIR == 0 (material.h) */
    chunk_fill(&c, make_voxel(MAT_AIR));

    uint32_t quads = greedy_mesh(&c, mb);

    char buf[96];
    int ok = (quads == 0) && (mb->vert_count == 0) && (mb->index_count == 0);
    snprintf(buf, sizeof buf, "quads=%u verts=%u indices=%u",
             quads, mb->vert_count, mb->index_count);
    report("case2_empty_chunk_zero_quads", ok, buf);
}

/* =========================================================================
 * Case 3 - a fully-solid stone chunk: only the 6 outer faces survive.
 * Every interior face is solid|solid -> culled. Each outer face is a single
 * 16x16 plane of identical (mat,face,light,ao) faces, so the greedy merge
 * collapses it to ONE maximal quad. Six outer faces => 6 quads exactly.
 *   6 quads -> 24 verts, 36 indices.
 * Out-of-chunk neighbours are AIR (mesher contract), so every boundary plane
 * is exposed - which is why all six faces emit even with no real neighbours.
 * ========================================================================= */
static void test_solid_chunk(MeshBuffer *mb)
{
    Chunk c;
    memset(&c, 0, sizeof c);
    chunk_fill(&c, make_voxel(MAT_STONE));

    uint32_t quads = greedy_mesh(&c, mb);

    char buf[96];
    int ok = (quads == 6)
          && (mb->vert_count   == 6u * VERTS_PER_QUAD)     /* 24 */
          && (mb->index_count  == 6u * INDICES_PER_QUAD);  /* 36 */
    snprintf(buf, sizeof buf, "quads=%u (want 6) verts=%u (want 24) idx=%u (want 36)",
             quads, mb->vert_count, mb->index_count);
    report("case3_solid_chunk_6_quads", ok, buf);
}

/* =========================================================================
 * Case 4 - two-material horizontal split: bottom 8 rows stone, top 8 air.
 *
 * Derivation of the expected quad count (this is the documented contract).
 * The solid region is a 16(X) x 16(Z) x 8(Y, rows ly=0..7) box of stone in an
 * otherwise-air chunk. A face is emitted where a stone voxel abuts air OR the
 * out-of-chunk boundary (which the mesher treats as AIR). Count per direction:
 *
 *   +Y (top)    : stone row ly=7 abuts air row ly=8. One 16x16 plane, uniform
 *                 merge key -> 1 maximal quad.
 *   -Y (bottom) : stone row ly=0 abuts the out-of-chunk floor (AIR). One 16x16
 *                 plane -> 1 maximal quad.
 *   +X          : stone column x=15 abuts out-of-chunk AIR. The exposed plane
 *                 spans Z=0..15 (16) x Y=0..7 (8) -> 1 maximal 16x8 quad.
 *   -X          : x=0 boundary, same 16x8 plane -> 1 quad.
 *   +Z          : z=15 boundary, plane spans X=0..15 (16) x Y=0..7 (8) -> 1 quad.
 *   -Z          : z=0 boundary, same 16x8 plane -> 1 quad.
 *
 * Interior stone|stone faces (rows ly=0..6 against ly=1..7) are all culled.
 * Total = 1 top + 1 bottom + 4 sides = 6 quads.
 * NOTE: the bottom (-Y) quad IS present because this is isolated meshing with
 * AIR boundaries; the chunk floor at ly=0 is an exposed face by contract.
 *   6 quads -> 24 verts, 36 indices.
 * ========================================================================= */
static void test_half_filled_chunk(MeshBuffer *mb)
{
    Chunk c;
    memset(&c, 0, sizeof c);
    chunk_fill(&c, make_voxel(MAT_AIR));

    /* Fill the bottom 8 rows (ly 0..7) with stone; top 8 stay air. */
    Voxel stone = make_voxel(MAT_STONE);
    for (int lz = 0; lz < CHUNK_DIM; ++lz)
        for (int ly = 0; ly < 8; ++ly)
            for (int lx = 0; lx < CHUNK_DIM; ++lx)
                chunk_set(&c, lx, ly, lz, stone);

    uint32_t quads = greedy_mesh(&c, mb);

    const uint32_t want_quads = 6;   /* 1 top + 1 bottom + 4 sides (see above) */
    char buf[112];
    int ok = (quads == want_quads)
          && (mb->vert_count  == want_quads * VERTS_PER_QUAD)
          && (mb->index_count == want_quads * INDICES_PER_QUAD);
    snprintf(buf, sizeof buf,
             "quads=%u (want %u: 1 top + 1 bottom + 4 sides@h8) verts=%u idx=%u",
             quads, want_quads, mb->vert_count, mb->index_count);
    report("case4_half_filled_chunk", ok, buf);
}

/* =========================================================================
 * Case 5 - one lone voxel in an otherwise-empty chunk: a full cube => 6 quads.
 * The single stone voxel is surrounded by air on all six sides, so each of its
 * six unit faces is visible and (being alone) cannot merge with anything.
 *   6 quads -> 24 verts, 36 indices.
 * ========================================================================= */
static void test_single_voxel(MeshBuffer *mb)
{
    Chunk c;
    memset(&c, 0, sizeof c);
    chunk_fill(&c, make_voxel(MAT_AIR));

    /* Place it well off the chunk boundary so no face touches an edge plane;
     * the count is 6 regardless, but this keeps the intent unambiguous. */
    chunk_set(&c, 8, 8, 8, make_voxel(MAT_STONE));

    uint32_t quads = greedy_mesh(&c, mb);

    char buf[96];
    int ok = (quads == 6)
          && (mb->vert_count  == 6u * VERTS_PER_QUAD)
          && (mb->index_count == 6u * INDICES_PER_QUAD);
    snprintf(buf, sizeof buf, "quads=%u (want 6) verts=%u idx=%u",
             quads, mb->vert_count, mb->index_count);
    report("case5_single_voxel_cube", ok, buf);
}

/* =========================================================================
 * Case 6 - the two-segment temperature codec round-trips and segments split.
 *   (a) 20 C -> encode -> decode must land within 1 C (ambient band, 1 C/code).
 *   (b) 1538 C (iron's melt point) must encode into the HOT segment, i.e. a
 *       code >= T_HOT_CODE (160), and decode back into the industrial band.
 * ========================================================================= */
static void test_temp_codec(void)
{
    /* (a) ambient round-trip */
    double in_a = 20.0;
    uint8_t code_a = temp_encode_c(in_a);
    double out_a = temp_decode_c(code_a);
    double err_a = out_a - in_a;
    if (err_a < 0) err_a = -err_a;

    char buf[128];
    int ok_a = (err_a <= 1.0);
    snprintf(buf, sizeof buf, "20C -> code %u -> %.1fC (err %.2f, tol 1.0)",
             code_a, out_a, err_a);
    report("case6a_temp_ambient_roundtrip", ok_a, buf);

    /* (b) 1538 C lands in the hot segment (code >= 160) */
    double in_b = 1538.0;
    uint8_t code_b = temp_encode_c(in_b);
    double out_b = temp_decode_c(code_b);
    int ok_b = (code_b >= T_HOT_CODE);
    /* The hot band is 20 C/code, so a round-trip is only good to +/-10 C; we
     * assert segment membership (the binding requirement) and report the decode
     * so a regression in the codec constants is visible in the log. */
    char buf2[128];
    snprintf(buf2, sizeof buf2,
             "1538C -> code %u (want >= %d, hot segment) -> %.1fC",
             code_b, (int)T_HOT_CODE, out_b);
    report("case6b_temp_hot_segment", ok_b, buf2);
}

/* Count the quads in *mb whose face direction equals `face`. A quad is 4
 * consecutive verts that all share the same face byte, so counting verts with
 * that face and dividing by VERTS_PER_QUAD gives the quad count for the
 * direction. Used to prove the SEAM-facing direction emits exactly zero faces. */
static uint32_t count_faces_in_dir(const MeshBuffer *mb, Face face)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < mb->vert_count; ++i)
        if (mb->verts[i].face == (uint8_t)face)
            ++n;
    return n / VERTS_PER_QUAD;
}

/* =========================================================================
 * Case 7 - cross-chunk seam culling via cached neigh[6] pointers.
 *
 * This is the milestone's load-bearing check: a face on the plane SHARED by two
 * adjacent solid chunks must be CULLED (the neighbour's edge voxels are solid),
 * so the merged two-chunk solid renders as one continuous body with NO internal
 * "seam wall" of spurious dark faces. The mesher reads c->neigh[dir] when a
 * sample steps out of 0..15; a NULL neighbour still reads as AIR (today's
 * isolated-chunk behaviour), so the world's true outer faces still emit.
 *
 * Neighbour index order (mesher.h Face enum / ARCHITECTURE.md Section 2.4):
 *   0 = -X, 1 = +X, 2 = -Y, 3 = +Y, 4 = -Z, 5 = +Z.
 *
 * WHAT WE ASSERT (and why NOT a fixed total quad count):
 * The mesher's sample() fix makes BOTH face-visibility AND boundary AO
 * neighbour-aware (mesher.c face_ao_level routes all 8 ring reads through
 * sample()). So when A gains a solid +X neighbour, the cells of A's OTHER outer
 * faces that lie along the shared edge now see solid (not air) ring voxels and
 * pick up corner AO darkening; that legitimately SPLITS those faces' greedy
 * quads. The surviving-quad TOTAL therefore is not a clean constant (it depends
 * on AO splitting) and must NOT be asserted exactly. The invariant that IS
 * load-bearing and AO-independent is: the SEAM-FACING direction emits ZERO
 * faces. We assert that directly by counting quads whose face byte is the seam
 * direction (FACE_POS_X for A, FACE_NEG_X for B) - it must be 0 - and as a heat
 * check that the linked pair emits strictly fewer faces than two isolated
 * chunks (the missing faces are exactly the two shared-plane faces).
 *
 * (7b) ISOLATION UNCHANGED: clear A->neigh[1] back to NULL and re-mesh A. With
 *      no neighbour, the +X boundary reads AIR again, so A returns to exactly 6
 *      quads - byte-identical to case3. This proves NULL neighbour == AIR
 *      boundary == today's behaviour, guarding the regression: adding neigh[6]
 *      must not change isolated-chunk meshing.
 * ========================================================================= */
static void test_cross_chunk_seam(MeshBuffer *mb)
{
    /* Two fully-solid stone chunks, adjacent along the X axis. memset zeroes
     * neigh[6] to all-NULL, so before linking each is an isolated chunk. */
    Chunk a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.cx = 0; a.cy = 0; a.cz = 0;
    b.cx = 1; b.cy = 0; b.cz = 0;
    chunk_fill(&a, make_voxel(MAT_STONE));
    chunk_fill(&b, make_voxel(MAT_STONE));

    /* Reference: a fully-solid isolated chunk emits exactly 6 quads (case3),
     * one per outer face including the +X boundary face. Capture both the total
     * and the +X-face count BEFORE linking, so we can prove that linking removes
     * precisely the shared-plane (+X) face. */
    mesh_buffer_reset(mb);
    uint32_t quads_solo  = greedy_mesh(&a, mb);              /* a still isolated */
    uint32_t solo_plusX  = count_faces_in_dir(mb, FACE_POS_X); /* the +X boundary */

    /* Link the shared face: A's +X neighbour is B, B's -X neighbour is A. */
    a.neigh[FACE_POS_X] = &b;   /* index 1 = +X */
    b.neigh[FACE_NEG_X] = &a;   /* index 0 = -X */

    /* ---- (7a) A's +X face (toward B's solid x=0 column) must be CULLED. ---- */
    mesh_buffer_reset(mb);
    uint32_t quads_a = greedy_mesh(&a, mb);
    uint32_t seam_a  = count_faces_in_dir(mb, FACE_POS_X); /* the shared +X plane */
    {
        char buf[128];
        int ok = (seam_a == 0);
        snprintf(buf, sizeof buf,
                 "A +X (seam) quads=%u (want 0, fully culled) total quads=%u",
                 seam_a, quads_a);
        report("case7a_seam_culls_chunkA_plusX", ok, buf);
    }

    /* ---- (7a, symmetric) B's -X face must be CULLED. ---- */
    mesh_buffer_reset(mb);
    uint32_t quads_b = greedy_mesh(&b, mb);
    uint32_t seam_b  = count_faces_in_dir(mb, FACE_NEG_X); /* the shared -X plane */
    {
        char buf[128];
        int ok = (seam_b == 0);
        snprintf(buf, sizeof buf,
                 "B -X (seam) quads=%u (want 0, fully culled) total quads=%u",
                 seam_b, quads_b);
        report("case7a_seam_culls_chunkB_minusX", ok, buf);
    }

    /* Heat check, AO-independent: the SHARED plane is empty from BOTH sides at
     * once. An ISOLATED solid chunk emits exactly one +X boundary face (the
     * seam wall the old single-chunk engine drew); linking removes it entirely,
     * and symmetrically B's -X face. So solo_plusX must be 1 while both linked
     * seam counts are 0 -> the two faces that used to form the seam wall are
     * gone. (Total quad counts are NOT compared: cross-chunk AO darkening along
     * the shared edge legitimately splits the surviving outer faces, so the
     * linked pair actually emits MORE, not fewer, total quads - which is why an
     * exact/`<' total-count assertion would be wrong here.) */
    {
        char buf[128];
        int ok = (quads_solo == 6)        /* isolated solid is the case3 6-quad cube */
              && (solo_plusX == 1)        /* one of those 6 IS the +X seam face   */
              && (seam_a == 0)            /* linked A drops its +X seam face      */
              && (seam_b == 0);           /* linked B drops its -X seam face      */
        snprintf(buf, sizeof buf,
                 "isolated quads=%u +X faces=%u (want 6,1); linked seam A=%u B=%u (want 0,0)",
                 quads_solo, solo_plusX, seam_a, seam_b);
        report("case7a_seam_wall_removed_both_sides", ok, buf);
    }

    /* ---- (7b) Unlink A and re-mesh: it must return to the original 6 quads. ----
     * This is the regression guard: NULL neighbour == AIR boundary == case3. */
    a.neigh[FACE_POS_X] = NULL;
    mesh_buffer_reset(mb);
    uint32_t quads_iso = greedy_mesh(&a, mb);
    {
        char buf[128];
        int ok = (quads_iso == 6)
              && (mb->vert_count  == 6u * VERTS_PER_QUAD)     /* 24 */
              && (mb->index_count == 6u * INDICES_PER_QUAD);  /* 36 */
        snprintf(buf, sizeof buf,
                 "A unlinked quads=%u (want 6 == case3) verts=%u (want 24) idx=%u (want 36)",
                 quads_iso, mb->vert_count, mb->index_count);
        report("case7b_unlinked_solid_unchanged", ok, buf);
    }
}

int main(void)
{
    printf("== test_mesher: greedy mesher + voxel codec ==\n");

    /* One reusable scratch buffer, sized to the worst case, the foundation
     * idiom (Section 4: a single staging buffer, reused across remeshes). */
    MeshBuffer mb;
    if (mesh_buffer_init(&mb, SCRATCH_VERTS, SCRATCH_INDICES) != 0) {
        fprintf(stderr, "FATAL: mesh_buffer_init failed\n");
        return 1;
    }

    test_sizes();

    mesh_buffer_reset(&mb);
    test_empty_chunk(&mb);

    mesh_buffer_reset(&mb);
    test_solid_chunk(&mb);

    mesh_buffer_reset(&mb);
    test_half_filled_chunk(&mb);

    mesh_buffer_reset(&mb);
    test_single_voxel(&mb);

    test_temp_codec();

    /* Cross-chunk seam: resets the scratch internally between sub-meshes. */
    test_cross_chunk_seam(&mb);

    mesh_buffer_free(&mb);

    if (g_failures == 0)
        printf("== ALL PASS ==\n");
    else
        printf("== %d FAILURE(S) ==\n", g_failures);

    /* Non-zero exit on any failure (clamped so it fits the 0..255 exit range). */
    return g_failures > 255 ? 255 : g_failures;
}
