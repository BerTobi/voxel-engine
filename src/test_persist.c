/* test_persist.c - Standalone unit harness for M8 world persistence: the
 * PersistStore (persist.h/persist.c) palette+RLE chunk record codec and the
 * 32x32x16 region-file store - the "store the player's DELTA from the seed"
 * layer that lets player/sim EDITS survive chunk eviction AND a process
 * restart, while UNMODIFIED chunks keep costing zero disk bytes (regenerated
 * from the deterministic worldgen seed).
 *
 * Binding source: persist.h (THE CONTRACT - the save format, the palette+RLE
 * record layout, the region-file header+index layout, and the persist_* API)
 * read together with ARCHITECTURE.md Section 8 (World Persistence), Section 2.1
 * (which voxel fields are authored STATE vs mesh-time DERIVED) and worldgen.h
 * (WG_GEN_VERSION, the gen-vs-stored principle). This file is PURE C: it touches
 * no GL and no OS beyond portable stdio + the save-dir the store creates, so it
 * builds and runs on the Linux dev host and the XP MinGW target alike. It drives
 * persist.c through a UNIQUE TEMP save dir, created+cleaned at start, exactly as
 * persist.h's "temp-dir testable" promise intends.
 *
 * It is its own test runner (zero external deps), in the test_world.c /
 * test_mesher.c idiom: each case prints "PASS: <name>" or "FAIL: <name> (why)";
 * the process returns the number of FAILED cases (0 == all green), which a CI
 * script can branch on.
 *
 * Build + run (from project root), exactly as the milestone brief states:
 *   gcc -std=c99 -Wall -Isrc -o build/m8_test \
 *       src/material.c src/chunk.c src/worldgen.c src/persist.c \
 *       src/test_persist.c -lm && build/m8_test
 *
 * THE TEST PLAN (persist.h API contract + the M8 brief, point for point):
 *   1. SAVE->LOAD round-trip of a MODIFIED chunk is byte-identical on the
 *      persisted fields mat|temp|fill|flags-as-state ... i.e. on the canonical
 *      bits PERSIST_VOX_MASK; light/ao (and the non-persisted flag bits) are
 *      IGNORED, since light.c rebakes them and the CA re-wakes from state.
 *   2. An UNMODIFIED coord (never saved) load returns 0 (MISS -> regenerate).
 *   3. palette+RLE round-trips a realistic mixed chunk, and a UNIFORM chunk
 *      saves MUCH smaller than the 16 KiB raw voxel array (the compression win).
 *   4. MULTIPLE chunks that share ONE region file each reload independently.
 *   5. A coord NEVER saved (but inside a region that DOES hold others) loads 0.
 *   6. RESTART DURABILITY: persist_close() the store, persist_open() a FRESH
 *      handle on the SAME save dir -> every prior save still loads identically.
 *   7. WORST CASE: a fully-heterogeneous chunk (every voxel distinct) round-
 *      trips, and its on-disk record stays within PERSIST_RECORD_MAX_BYTES (the
 *      RAW escape-hatch cap - palette+RLE must never be allowed to bloat past
 *      raw).
 *   8. The NULL store is a no-op: save/load/flush on a NULL PersistStore are the
 *      "persistence disabled" path (today's ephemeral M7 behaviour) and never
 *      crash; load reports MISS.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "voxel.h"
#include "chunk.h"
#include "material.h"
#include "worldgen.h"
#include "persist.h"

/* ---- Tiny assertion plumbing (same idiom as test_world.c/test_mesher.c) -- */
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

/* ======================================================================== *
 *  Temp save directory: created + cleaned at start, removed at the end.     *
 * ======================================================================== *
 * persist.h promises temp-dir testability. We use a fixed path under build/
 * (portable: relative to the cwd the brief runs from). To make a CLEAN start
 * we delete any region files left by a prior run; persist_open() (re)creates
 * the dir via its private #ifdef _WIN32 _mkdir / POSIX mkdir helper. We avoid
 * any OS-specific recursive-delete: only this test's own .dat files live here,
 * and removing them is enough for a deterministic run. */
#define TEST_SAVE_DIR "build/persist_test"

/* Remove the known region files this test can create, so each run starts from
 * a clean slate regardless of what a prior run left behind. We brute-force a
 * small window of region coords around the origin (the only coords this test
 * ever touches) rather than enumerate the directory (no portable opendir on
 * the XP MinGW target's headers without extra ifdefs). */
static void clean_save_dir(void)
{
    int rx, rz;
    char path[256];
    for (rz = -2; rz <= 2; ++rz) {
        for (rx = -2; rx <= 2; ++rx) {
            snprintf(path, sizeof path, "%s/r.%d.%d.dat", TEST_SAVE_DIR, rx, rz);
            remove(path);   /* ENOENT is fine: nothing to clean */
        }
    }
}

/* ======================================================================== *
 *  Chunk builders (deterministic, in the chunk.c voxel-codec idiom)         *
 * ======================================================================== */

/* Stamp light/ao/flags bits into a voxel so the round-trip can PROVE the codec
 * drops them (persist_canon masks to PERSIST_VOX_MASK). These are the derived/
 * transient bits that must NOT survive persistence. */
static Voxel with_derived_noise(Voxel v, uint8_t light, uint8_t ao, uint8_t fl)
{
    vox_set_light(&v, light);
    vox_set_ao(&v, ao);
    vox_set_flags(&v, fl);
    return v;
}

/* 0.5 M1: Chunk.voxels is a pointer now, so a stack/scratch chunk needs a backing
 * 16 KiB block. Hand each test chunk a fresh one (leak at exit is fine in a unit
 * test). `poison` fills the block with 0xAB so a "real load must overwrite"
 * assertion still holds; the chunk record itself is poisoned/zeroed by the caller. */
static void chunk_back(Chunk *c, int poison)
{
    c->voxels   = (Voxel *)malloc((size_t)CHUNK_VOXELS * sizeof(Voxel));
    c->slab_idx = -1;
    memset(c->voxels, poison ? 0xAB : 0x00, (size_t)CHUNK_VOXELS * sizeof(Voxel));
}

/* Build a UNIFORM modified chunk: every voxel identical (one material, one
 * temp, one fill), with non-zero light/ao/flags noise sprinkled on so the
 * canonicalisation is exercised. This is the best-case compression target:
 * one palette entry, one RLE run. */
static void build_uniform_chunk(Chunk *c, int cx, int cy, int cz,
                                 uint8_t mat, double temp_c, uint8_t fill)
{
    Voxel base = 0;
    int i;
    memset(c, 0, sizeof *c);
    chunk_back(c, 0);                   /* 0.5 M1: voxels is a pointer - back it */
    c->cx = cx; c->cy = cy; c->cz = cz;
    vox_set_mat(&base, mat);
    vox_set_temp_code(&base, temp_encode_c(temp_c));
    vox_set_fill(&base, fill);
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        /* vary ONLY the derived/transient bits voxel-to-voxel: canon must
         * collapse them all to the SAME palette entry / one run. */
        c->voxels[i] = with_derived_noise(base,
                                          (uint8_t)(i & 0x0F),
                                          (uint8_t)((i >> 1) & 0x0F),
                                          (uint8_t)(i & 0x0F));
    }
    c->flags = CHUNK_MODIFIED | CHUNK_DIRTY_MESH;
}

/* Build a REALISTIC mixed modified chunk: a worldgen-style stratified column
 * (stone bulk, dirt cap, an air void carved out, a small ore pocket and a
 * puddle of half-full water) - long runs in the native x,y,z linear order, a
 * handful of distinct materials. Exactly the shape the palette+RLE scheme is
 * tuned for. Derived bits are again noised to prove they drop out. */
static void build_mixed_chunk(Chunk *c, int cx, int cy, int cz)
{
    uint8_t ambient = temp_encode_c(20.0);
    int lx, ly, lz;
    memset(c, 0, sizeof *c);
    chunk_back(c, 0);                   /* 0.5 M1: voxels is a pointer - back it */
    c->cx = cx; c->cy = cy; c->cz = cz;

    for (lz = 0; lz < CHUNK_DIM; ++lz) {
        for (ly = 0; ly < CHUNK_DIM; ++ly) {
            for (lx = 0; lx < CHUNK_DIM; ++lx) {
                Voxel v = 0;
                if (ly < 8) {
                    vox_set_mat(&v, MAT_STONE);
                    vox_set_fill(&v, 15);
                    vox_set_temp_code(&v, ambient);
                } else if (ly < 11) {
                    vox_set_mat(&v, MAT_DIRT);
                    vox_set_fill(&v, 15);
                    vox_set_temp_code(&v, ambient);
                } else {
                    /* air above the surface (mat 0, fill 0) */
                    vox_set_temp_code(&v, ambient);
                }
                /* an ore pocket: a small solid cube near a corner */
                if (lx < 3 && ly >= 3 && ly < 6 && lz < 3) {
                    vox_set_mat(&v, MAT_IRON_ORE);
                    vox_set_fill(&v, 15);
                    /* hotter: a vein the player has been smelting near */
                    vox_set_temp_code(&v, temp_encode_c(140.0));
                }
                /* a half-full water puddle sitting on the dirt cap */
                if (ly == 11 && lx >= 8 && lz >= 8) {
                    vox_set_mat(&v, MAT_WATER);
                    vox_set_fill(&v, 7);
                    vox_set_temp_code(&v, ambient);
                }
                v = with_derived_noise(v,
                                       (uint8_t)((lx + ly) & 0x0F),
                                       (uint8_t)((lz + ly) & 0x0F),
                                       (uint8_t)(lx & 0x0F));
                c->voxels[vox_index(lx, ly, lz)] = v;
            }
        }
    }
    c->flags = CHUNK_MODIFIED | CHUNK_DIRTY_MESH;
}

/* Build the WORST CASE chunk: try to make every one of the 4096 CANONICAL words
 * distinct, defeating both palette and RLE. We have 20 persistable bits
 * (PERSIST_VOX_MASK = mat|temp|fill = 0x000FFFFF), and 4096 < 2^20, so 4096
 * distinct canonical words DO exist: spread the voxel index across mat(8),
 * temp(8) and fill(4). This forces palette_count > PERSIST_PALETTE_MAX (256),
 * so persist.c MUST fall back to PERSIST_COMPRESS_RAW - the bounded escape
 * hatch. We keep mat in a sane range so material_of() never reads out of the
 * table, though persistence does not interpret the material. */
static void build_worst_case_chunk(Chunk *c, int cx, int cy, int cz)
{
    int i;
    memset(c, 0, sizeof *c);
    chunk_back(c, 0);                   /* 0.5 M1: voxels is a pointer - back it */
    c->cx = cx; c->cy = cy; c->cz = cz;
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        Voxel v = 0;
        /* i in 0..4095 -> 12 bits. Scatter into mat/temp/fill so canonical
         * words are all distinct: low 4 bits -> fill, next 8 bits -> temp,
         * and a rolling material keeps mat varying too. */
        uint8_t fill = (uint8_t)(i & 0x0F);
        uint8_t temp = (uint8_t)((i >> 4) & 0xFF);
        uint8_t mat  = (uint8_t)((i & 0x07) + 1);   /* 1..8, valid table ids */
        vox_set_mat(&v, mat);
        vox_set_temp_code(&v, temp);
        vox_set_fill(&v, fill);
        /* noise the derived bits too */
        v = with_derived_noise(v, (uint8_t)(i & 0x0F),
                               (uint8_t)((i >> 2) & 0x0F),
                               (uint8_t)(i & 0x0F));
        c->voxels[i] = v;
    }
    c->flags = CHUNK_MODIFIED | CHUNK_DIRTY_MESH;
}

/* ======================================================================== *
 *  Comparison helpers                                                       *
 * ======================================================================== */

/* Do two chunks match on the PERSISTED fields only (mat|temp|fill, i.e.
 * PERSIST_VOX_MASK)? light/ao/flags are derived/transient and must be IGNORED
 * (persist.h: they are masked to 0 before palettizing and rebaked on load). On
 * a mismatch, fill `detail` with the first offending voxel. Returns 1 if equal
 * on the canonical bits. */
static int chunks_equal_canon(const Chunk *a, const Chunk *b,
                              char *detail, size_t detail_sz)
{
    int i;
    for (i = 0; i < CHUNK_VOXELS; ++i) {
        Voxel ca = persist_canon(a->voxels[i]);
        Voxel cb = persist_canon(b->voxels[i]);
        if (ca != cb) {
            snprintf(detail, detail_sz,
                     "voxel[%d] canon 0x%08lX != 0x%08lX",
                     i, (unsigned long)ca, (unsigned long)cb);
            return 0;
        }
    }
    return 1;
}

/* On-disk byte size of the record for chunk slot (cx,cy,cz) in its region file:
 * read the region index entry and return sector_count * REGION_SECTOR (0 if not
 * stored / file absent). Used to prove the uniform chunk compresses small and
 * the worst case stays under the cap. We read the index slot directly so the
 * test is independent of any internal persist.c accounting. */
static long record_disk_bytes(const char *save_dir, int cx, int cy, int cz)
{
    char path[256];
    FILE *f;
    RegionIndexEntry e;
    long off;
    int32_t rx = region_coord_x(cx);
    int32_t rz = region_coord_z(cz);
    uint32_t slot = region_slot(cx, cy, cz);

    snprintf(path, sizeof path, "%s/r.%d.%d.dat", save_dir, rx, rz);
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;                      /* region file absent */

    off = (long)sizeof(RegionHeader) + (long)slot * (long)sizeof(RegionIndexEntry);
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    if (fread(&e, sizeof e, 1, f) != 1) { fclose(f); return -1; }
    fclose(f);
    return (long)e.sector_count * (long)REGION_SECTOR;
}

/* ======================================================================== *
 *  Test cases                                                               *
 * ======================================================================== */

/* World identity shared by every case (one save belongs to one seed/world). */
#define TEST_SEED  ((uint64_t)0xBA15C0DEull)

/* Case 1 - modified chunk SAVE->LOAD round-trip, byte-identical on the
 * persisted fields (mat|temp|fill); light/ao/flags ignored. */
static void test_roundtrip_modified(PersistStore *ps)
{
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[160]; int ok = 1; buf[0] = '\0';

    if (src == NULL || dst == NULL) {
        report("modified chunk save->load round-trip", 0, "OOM");
        free(src); free(dst); return;
    }

    build_mixed_chunk(src, 2, 1, 3);
    if (persist_save_chunk(ps, src) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "persist_save_chunk failed");
    }

    if (ok) {
        memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);     /* poison: a real load must overwrite */
        int hit = persist_load_chunk(ps, dst, 2, 1, 3);
        if (hit != 1) {
            ok = 0; snprintf(buf, sizeof buf, "load returned %d, expected 1 (HIT)", hit);
        } else if (dst->cx != 2 || dst->cy != 1 || dst->cz != 3) {
            ok = 0; snprintf(buf, sizeof buf, "loaded coords (%d,%d,%d) != (2,1,3)",
                             dst->cx, dst->cy, dst->cz);
        } else if (!chunks_equal_canon(src, dst, buf, sizeof buf)) {
            ok = 0;     /* buf already filled with the offending voxel */
        }
    }
    report("modified chunk save->load round-trip (mat|temp|fill)", ok, buf);
    free(src); free(dst);
}

/* Case 2 - an UNMODIFIED coord never saved loads 0 (MISS -> caller regenerates). */
static void test_unmodified_miss(PersistStore *ps)
{
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[128]; int ok = 1; buf[0] = '\0';
    if (dst == NULL) { report("unmodified coord load returns MISS", 0, "OOM"); return; }

    /* A coord we never saved (and far from any saved one). */
    memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
    int hit = persist_load_chunk(ps, dst, 9, 0, 9);
    if (hit != 0) { ok = 0; snprintf(buf, sizeof buf, "load returned %d, expected 0 (MISS)", hit); }
    report("unmodified coord load returns 0 (MISS)", ok, buf);
    free(dst);
}

/* Case 3 - palette+RLE round-trips a mixed chunk AND a uniform chunk stores
 * MUCH smaller than the 16 KiB raw voxel array. */
static void test_uniform_compresses(PersistStore *ps)
{
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[192]; int ok = 1; buf[0] = '\0';
    if (src == NULL || dst == NULL) {
        report("uniform chunk compresses well", 0, "OOM");
        free(src); free(dst); return;
    }

    /* A solid block of hot iron at one fill level: one palette entry, one run. */
    build_uniform_chunk(src, -1, 0, -1, MAT_IRON, 800.0, 15);
    if (persist_save_chunk(ps, src) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "save failed");
    }

    if (ok) {
        memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
        if (persist_load_chunk(ps, dst, -1, 0, -1) != 1) {
            ok = 0; snprintf(buf, sizeof buf, "uniform reload MISS");
        } else if (!chunks_equal_canon(src, dst, buf, sizeof buf)) {
            ok = 0;
        }
    }

    if (ok) {
        long bytes = record_disk_bytes(TEST_SAVE_DIR, -1, 0, -1);
        long raw   = (long)CHUNK_VOXELS * 4;   /* 16384 */
        /* A uniform chunk is one palette entry + one RLE run + a 28 B header -
         * well under a hundred bytes of actual payload. Records are SECTOR-
         * aligned (persist.h), so the smallest achievable on-disk size is ONE
         * REGION_SECTOR (4096 B) = exactly a quarter of the 16 KiB raw array.
         * Proving it fits in a single sector both confirms the compression win
         * (4x smaller, the minimum possible record) and is robust to the
         * sector-rounding the format mandates. */
        if (bytes < 0) {
            ok = 0; snprintf(buf, sizeof buf, "could not read record size");
        } else if (bytes > (long)REGION_SECTOR) {
            ok = 0; snprintf(buf, sizeof buf,
                "uniform record %ld B exceeds one sector (%u B); not << raw %ld B",
                bytes, (unsigned)REGION_SECTOR, raw);
        } else {
            printf("      (uniform chunk on disk: %ld B = %ld sector(s) vs %ld B raw)\n",
                   bytes, bytes / (long)REGION_SECTOR, raw);
        }
    }
    report("uniform chunk palette+RLE compresses << 16 KiB", ok, buf);
    free(src); free(dst);
}

/* Case 4 - multiple chunks SHARE one region file and each reloads independently;
 * Case 5 - a coord never saved but inside that SAME region loads 0. */
static void test_multi_chunk_region(PersistStore *ps)
{
    /* Three coords that all map to region (0,0): cx,cz in 0..31. Different cy
     * so they occupy distinct slots in the SAME region file. */
    static const int coords[][3] = { {1,0,1}, {1,1,1}, {5,0,9} };
    int n = 3, i, ok = 1;
    char buf[192]; buf[0] = '\0';
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    if (src == NULL || dst == NULL) {
        report("multiple chunks share a region", 0, "OOM");
        free(src); free(dst); return;
    }

    /* Confirm the shared-region assumption holds for our chosen coords. */
    for (i = 0; i < n && ok; ++i) {
        if (region_coord_x(coords[i][0]) != 0 || region_coord_z(coords[i][2]) != 0) {
            ok = 0; snprintf(buf, sizeof buf, "coord %d not in region (0,0)", i);
        }
    }

    for (i = 0; i < n && ok; ++i) {
        build_uniform_chunk(src, coords[i][0], coords[i][1], coords[i][2],
                            (uint8_t)(MAT_STONE + i), 20.0 + 30.0 * i, 15);
        if (persist_save_chunk(ps, src) != 0) {
            ok = 0; snprintf(buf, sizeof buf, "save of coord %d failed", i);
        }
    }

    /* Reload each and verify it matches what was written for THAT coord. */
    for (i = 0; i < n && ok; ++i) {
        build_uniform_chunk(src, coords[i][0], coords[i][1], coords[i][2],
                            (uint8_t)(MAT_STONE + i), 20.0 + 30.0 * i, 15);
        memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
        if (persist_load_chunk(ps, dst, coords[i][0], coords[i][1], coords[i][2]) != 1) {
            ok = 0; snprintf(buf, sizeof buf, "coord %d reload MISS", i);
        } else if (!chunks_equal_canon(src, dst, buf, sizeof buf)) {
            ok = 0;
        }
    }
    report("multiple chunks share a region, each reloads", ok, buf);

    /* Case 5: a coord in region (0,0) we never saved -> MISS. */
    ok = 1; buf[0] = '\0';
    memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
    if (region_coord_x(20) != 0 || region_coord_z(20) != 0) {
        report("never-saved coord in a live region loads 0", 0, "(20,*,20) not in region 0,0");
    } else {
        int hit = persist_load_chunk(ps, dst, 20, 0, 20);
        if (hit != 0) { ok = 0; snprintf(buf, sizeof buf, "load returned %d, expected MISS", hit); }
        report("never-saved coord in a live region loads 0 (MISS)", ok, buf);
    }
    free(src); free(dst);
}

/* Case 7 - the fully-heterogeneous worst case round-trips AND its record stays
 * within the PERSIST_RECORD_MAX_BYTES cap (RAW escape hatch). */
static void test_worst_case(PersistStore *ps)
{
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[192]; int ok = 1; buf[0] = '\0';
    if (src == NULL || dst == NULL) {
        report("worst-case chunk round-trips within cap", 0, "OOM");
        free(src); free(dst); return;
    }

    build_worst_case_chunk(src, 10, 1, 10);
    if (persist_save_chunk(ps, src) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "save failed");
    }
    if (ok) {
        memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
        if (persist_load_chunk(ps, dst, 10, 1, 10) != 1) {
            ok = 0; snprintf(buf, sizeof buf, "worst-case reload MISS");
        } else if (!chunks_equal_canon(src, dst, buf, sizeof buf)) {
            ok = 0;
        }
    }
    if (ok) {
        long bytes = record_disk_bytes(TEST_SAVE_DIR, 10, 1, 10);
        long cap   = (long)PERSIST_RECORD_MAX_SECTORS * (long)REGION_SECTOR; /* 5*4096 */
        if (bytes < 0) {
            ok = 0; snprintf(buf, sizeof buf, "could not read record size");
        } else if (bytes > cap) {
            ok = 0; snprintf(buf, sizeof buf,
                "worst-case record %ld B exceeds cap %ld B", bytes, cap);
        } else {
            printf("      (worst-case chunk on disk: %ld B, cap %ld B)\n", bytes, cap);
        }
    }
    report("worst-case heterogeneous chunk round-trips within size cap", ok, buf);
    free(src); free(dst);
}

/* Case 6 - RESTART DURABILITY: close the store, reopen a FRESH handle on the
 * same save dir, and verify a chunk saved before the close still loads
 * identically. This is the whole point of persistence: edits survive a process
 * restart, not just an eviction. */
static void test_restart_durability(void)
{
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[192]; int ok = 1; buf[0] = '\0';
    PersistStore *ps1, *ps2;

    if (src == NULL || dst == NULL) {
        report("restart durability", 0, "OOM");
        free(src); free(dst); return;
    }

    ps1 = persist_open(TEST_SAVE_DIR, TEST_SEED, WG_GEN_VERSION);
    if (ps1 == NULL) {
        report("restart durability", 0, "persist_open(#1) failed");
        free(src); free(dst); return;
    }

    build_mixed_chunk(src, 7, 1, 4);
    if (persist_save_chunk(ps1, src) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "save before close failed");
    }
    /* Flush then fully close handle #1 - simulate process exit. */
    if (ok && persist_flush(ps1) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "persist_flush failed");
    }
    persist_close(ps1);

    if (ok) {
        /* Fresh handle, same dir/seed/gen: the new "process". */
        ps2 = persist_open(TEST_SAVE_DIR, TEST_SEED, WG_GEN_VERSION);
        if (ps2 == NULL) {
            ok = 0; snprintf(buf, sizeof buf, "persist_open(#2) failed");
        } else {
            memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
            if (persist_load_chunk(ps2, dst, 7, 1, 4) != 1) {
                ok = 0; snprintf(buf, sizeof buf, "reopened store: chunk MISS (lost on restart)");
            } else if (!chunks_equal_canon(src, dst, buf, sizeof buf)) {
                ok = 0;
            }
            persist_close(ps2);
        }
    }
    report("restart durability: saves survive close + reopen", ok, buf);
    free(src); free(dst);
}

/* Case 8 - NULL store is the "persistence disabled" no-op path: save/load/flush
 * must not crash and load must report MISS. */
static void test_null_store_noop(void)
{
    Chunk *c = malloc(sizeof(Chunk));
    char buf[128]; int ok = 1; buf[0] = '\0';
    if (c == NULL) { report("NULL store is a safe no-op", 0, "OOM"); return; }

    build_uniform_chunk(c, 0, 0, 0, MAT_STONE, 20.0, 15);
    if (persist_save_chunk(NULL, c) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "save(NULL) returned non-zero");
    }
    if (ok && persist_load_chunk(NULL, c, 0, 0, 0) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "load(NULL) returned a HIT, expected MISS");
    }
    if (ok && persist_flush(NULL) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "flush(NULL) returned non-zero");
    }
    persist_close(NULL);   /* must be a safe no-op */
    report("NULL store save/load/flush/close are safe no-ops", ok, buf);
    free(c);
}

/* Case 9 (0.2.1 regression) - persist_open MUST create missing PARENT dirs.
 * The default save dir is two levels deep ("saves/<seed>"); a single mkdir of
 * the leaf fails with ENOENT when the parent does not exist, which on a fresh
 * checkout silently disabled ALL persistence (the store stayed NULL, so every
 * edit was ephemeral and the player's builds regenerated from seed the moment a
 * chunk was evicted + reloaded - "what I build doesn't last, going around the
 * planet everything disappeared"). persist_open must "mkdir -p" the whole path. */
#define TEST_NESTED_PARENT "build/persist_test_nested"
#define TEST_NESTED_DIR    TEST_NESTED_PARENT "/seedsub"

static void test_nested_parent_mkdir(void)
{
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[192]; int ok = 1; buf[0] = '\0';
    PersistStore *ps;

    if (src == NULL || dst == NULL) {
        report("nested-parent mkdir", 0, "OOM"); free(src); free(dst); return;
    }

    /* Force the precondition: the PARENT dir must not exist before the open, so
     * a single-level mkdir of the leaf would fail (POSIX remove() rmdirs an
     * empty dir; ENOENT is fine). On a clean build, build/ exists but neither of
     * these does, which is exactly the live "saves/" case. */
    remove(TEST_NESTED_DIR "/r.0.0.dat");
    remove(TEST_NESTED_DIR);
    remove(TEST_NESTED_PARENT);

    ps = persist_open(TEST_NESTED_DIR, TEST_SEED, WG_GEN_VERSION);
    if (ps == NULL) {
        report("persist_open creates missing parent dirs (mkdir -p)", 0,
               "returned NULL on a 2-level path whose parent did not exist");
        free(src); free(dst); return;
    }

    build_mixed_chunk(src, 0, 0, 0);
    if (persist_save_chunk(ps, src) != 0) {
        ok = 0; snprintf(buf, sizeof buf, "save into nested dir failed");
    }
    if (ok) {
        memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
        if (persist_load_chunk(ps, dst, 0, 0, 0) != 1) {
            ok = 0; snprintf(buf, sizeof buf, "reload MISS from nested dir");
        } else if (!chunks_equal_canon(src, dst, buf, sizeof buf)) {
            ok = 0;
        }
    }
    persist_flush(ps);
    persist_close(ps);
    report("persist_open creates missing parent dirs (mkdir -p)", ok, buf);

    /* Leave the slate clean for re-runs. */
    remove(TEST_NESTED_DIR "/r.0.0.dat");
    remove(TEST_NESTED_DIR);
    remove(TEST_NESTED_PARENT);
    free(src); free(dst);
}

/* Case 10 (0.5 M6 compat gate) - a save from an OLDER generator version is
 * REFUSED on load, NOT silently mis-read as current data. This is the exact
 * "0.4 saves are refused, not migrated" guarantee: 0.4 stamped gen_version 2,
 * 0.5 expects WG_GEN_VERSION (3). region_read_meta refuses a gen mismatch
 * (persist.c:216) and the LOAD path treats a refused region as a MISS (the chunk
 * regenerates from the current seed/gen), so a 32 m-pebble 0.4 voxel can never
 * be re-interpreted as a 256 m-planet 0.5 voxel. We simulate the old save by
 * opening a store at gen_version-1 (== 2, the literal 0.4 value), then assert a
 * fresh store at the current gen sees the chunk as MISS. */
#define TEST_STALEGEN_DIR "build/persist_test_stalegen"

static void test_stale_gen_refused(void)
{
    Chunk *src = malloc(sizeof(Chunk));
    Chunk *dst = malloc(sizeof(Chunk));
    char buf[192]; int ok = 1; buf[0] = '\0';
    PersistStore *ps_old, *ps_new;
    const uint32_t OLD_GEN = WG_GEN_VERSION - 1u;   /* == 2 == the 0.4 gen version */

    if (src == NULL || dst == NULL) { report("stale-gen refused", 0, "OOM"); free(src); free(dst); return; }
    remove(TEST_STALEGEN_DIR "/r.0.0.dat");          /* clean slate for re-runs */

    /* (a) write a "0.4" save: a real modified chunk stamped at the OLD gen version. */
    ps_old = persist_open(TEST_STALEGEN_DIR, TEST_SEED, OLD_GEN);
    if (ps_old == NULL) { report("stale-gen refused", 0, "persist_open(old gen) failed"); free(src); free(dst); return; }
    build_mixed_chunk(src, 2, 3, 5);
    if (persist_save_chunk(ps_old, src) != 0) { ok = 0; snprintf(buf, sizeof buf, "save under old gen failed"); }
    if (ok && persist_flush(ps_old) != 0)     { ok = 0; snprintf(buf, sizeof buf, "flush under old gen failed"); }
    persist_close(ps_old);

    /* (b) a current-gen (0.5) store MUST refuse it -> load MISS, not a mis-read. */
    if (ok) {
        ps_new = persist_open(TEST_STALEGEN_DIR, TEST_SEED, WG_GEN_VERSION);
        if (ps_new == NULL) { ok = 0; snprintf(buf, sizeof buf, "persist_open(current gen) failed"); }
        else {
            memset(dst, 0xAB, sizeof *dst); chunk_back(dst, 1);
            if (persist_load_chunk(ps_new, dst, 2, 3, 5) != 0) {
                ok = 0; snprintf(buf, sizeof buf, "stale-gen chunk LOADED (mis-read!) - expected MISS (refused)");
            }
            persist_close(ps_new);
        }
    }
    report("stale-gen (0.4) save is REFUSED on load, not migrated", ok, buf);
    remove(TEST_STALEGEN_DIR "/r.0.0.dat");
    remove(TEST_STALEGEN_DIR);
    free(src); free(dst);
}

int main(void)
{
    PersistStore *ps;

    printf("=== M8 persistence tests (palette+RLE region store) ===\n");

    /* The NULL-store path needs no save dir; run it first. */
    test_null_store_noop();

    /* Clean + open the shared store the body of the suite uses. persist_open
     * (re)creates TEST_SAVE_DIR via its private mkdir helper. */
    clean_save_dir();
    ps = persist_open(TEST_SAVE_DIR, TEST_SEED, WG_GEN_VERSION);
    if (ps == NULL) {
        report("persist_open(temp dir)", 0, "returned NULL - cannot run suite");
        printf("=== %d failure(s) ===\n", g_failures);
        return g_failures;
    }
    report("persist_open(temp dir) succeeds", 1, NULL);

    test_roundtrip_modified(ps);
    test_unmodified_miss(ps);
    test_uniform_compresses(ps);
    test_multi_chunk_region(ps);
    test_worst_case(ps);

    /* Flush + close this store BEFORE the restart-durability case opens its own
     * fresh handles on the same dir (so there is no double-open of region
     * files). The data written above persists on disk for ps to reuse? No - the
     * restart case writes its OWN chunk; it just needs the dir to exist. */
    persist_flush(ps);
    persist_close(ps);

    test_restart_durability();
    test_nested_parent_mkdir();
    test_stale_gen_refused();   /* 0.5 M6: the "0.4 saves refused, not migrated" gate */

    printf("=== %d failure(s) ===\n", g_failures);
    return g_failures;
}
