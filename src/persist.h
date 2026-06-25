/* persist.h - World persistence: store the player's DELTA from the seed.
 *
 * Binding source: ARCHITECTURE.md Section 8 (World Persistence) - the save
 * format, the gen-vs-stored principle, the palette+RLE compression, and the
 * 32x32x16 region-file layout - read together with Section 2.1 (which voxel
 * fields are authored STATE vs mesh-time DERIVED) and Section 7 (the eviction /
 * load / shutdown points in world.c where this module is called).
 *
 * THE ONE IDEA (Section 8 "Store Deltas, Regenerate the Rest").
 * worldgen is a pure deterministic function gen(seed, cx, cy, cz) -> Chunk:
 * same seed + same coords -> byte-identical voxels, on any machine, forever
 * (worldgen.h DETERMINISM CONTRACT). So a chunk the player NEVER TOUCHED costs
 * ZERO disk bytes - on reload we just call gen() again. Persistence remembers
 * exactly one thing: WHERE THE PLAYER DIVERGED FROM THE SEED. Only a chunk
 * carrying CHUNK_MODIFIED (chunk.h 0x02, set the first time a non-generator
 * source alters any voxel in it) is ever written; everything else regenerates.
 * Save size therefore scales with player ACTIVITY, not explored distance - a
 * mature save is single-digit-to-low-double-digit MiB, never the 4.00 GiB
 * full-world figure (Section 8). This mirrors the in-RAM bounded-window
 * invariant (Section 7): flying admiring vistas costs nothing; only CHANGING
 * things costs bytes.
 *
 * WHERE THIS PLUGS INTO world.c (Section 7 hooks; world.c stays GL-free and the
 * persistence path is its own pure-C, temp-dir-testable module - this file):
 *   - world_insert / LOAD : try persist_load_chunk() FIRST; on a hit it restores
 *     mat|temp|fill verbatim and zeroes light|ao|flags (rebaked/re-woken on
 *     load). On a miss (the common case) world_insert falls through to cb.gen
 *     exactly as today. Either way the chunk is then lit + meshed normally.
 *   - world_evict / EVICT : the existing Section-8 hook in world_evict reads
 *         if (c->flags & CHUNK_MODIFIED) persist_save_chunk(ws->store, c);
 *     before the slab is returned to the pool. An UNMODIFIED chunk is dropped
 *     with zero I/O (the seed is its storage). This is the branch world.c's M7
 *     comment reserved ("the future if(CHUNK_MODIFIED) enqueue_writeback ...").
 *   - world_shutdown / FLUSH : persist_flush() + persist_close() write every
 *     resident modified chunk and close all open region handles, so edits
 *     survive a process restart, not just an eviction.
 * world.c owns one PersistStore* (or NULL = "no save dir, edits are ephemeral",
 * exactly today's M7 behaviour - persistence is OPTIONAL and INJECTED, never
 * mandatory, so test_world.c keeps running with no save dir).
 *
 * PORTABILITY (this is the engine's FIRST disk I/O). Pure C99 stdio only -
 * fopen("wb"/"rb"), fread, fwrite, fseek/ftell - which behaves identically on
 * the Linux dev host and the XP MinGW target. The only OS-specific call is
 * creating the save directory; persist.c hides it behind a one-line helper
 *     #ifdef _WIN32  _mkdir(p)  #else  mkdir(p, 0777)  #endif
 * declared private to persist.c. Nothing else here is OS-specific.
 *
 * ENDIANNESS. The dev box (x86-64) and the XP target (Pentium M) are BOTH
 * little-endian x86, so the on-disk format is fixed little-endian and we write
 * raw u32 voxel words / struct fields DIRECTLY with fwrite, no byte-swapping.
 * This assumption is stamped (REGION_MAGIC + version) so a future big-endian
 * port is REFUSED at load rather than silently mis-read; it is not handled now.
 *
 * SINGLE-THREADED. No locks, no async I/O. A flush is a seek + write of a few
 * sectors to an already-open handle; it slots into the frame's overhead/slack
 * band (Section 8 "persistence is elastic, like meshing, never a hitch").
 */
#ifndef PERSIST_H
#define PERSIST_H

#include <stdint.h>
#include "voxel.h"   /* Voxel, CHUNK_VOXELS, VOX_* masks (field codec)         */
#include "chunk.h"   /* Chunk, CHUNK_MODIFIED                                  */

/* ======================================================================== *
 *  0. FORMAT VERSIONS                                                       *
 * ======================================================================== *
 * Two INDEPENDENT version numbers travel in every region header (Section 8
 * "the generator must be versioned" + "Version the format"):
 *
 *  - PERSIST_FORMAT_VERSION : the on-disk LAYOUT (header/index/record byte
 *    shape, the compression token grammar). Bump when THIS file's structs or
 *    the RLE stream grammar change. A reader refuses a newer format it does not
 *    understand (forward-compat gate).
 *
 *  - gen_version (stamped from worldgen.h WG_GEN_VERSION, NOT defined here):
 *    the GENERATOR identity. Section 8's correctness trap: if the noise ever
 *    changes, a previously-UNMODIFIED chunk would regenerate DIFFERENTLY than
 *    the player last saw it - a cliff they walked past silently mutates. So the
 *    region header stamps WG_GEN_VERSION at write time; the default policy
 *    (Section 8, binding) on a mismatch is REFUSE TO LOAD ("this save belongs
 *    to an older generator") rather than silently corrupt the player's memory
 *    of their world. Bulk-rebake-on-upgrade is explicitly out of scope. */
#define PERSIST_FORMAT_VERSION  2u   /* 0.5 M2: grain flip - 0.4 (v1) saves are refused */

/* ======================================================================== *
 *  1. REGION GEOMETRY  (Section 8 "Region Files", binding numbers)          *
 * ======================================================================== *
 * DECISION: batch chunks into REGION FILES of 32 x 32 chunk columns across the
 * FULL 16-layer vertical band - one file owns a 32 x 32 x 16 = 16,384-chunk
 * volume (a 512 m x 512 m x 256 m world column). NOT one-file-per-chunk.
 *
 * WHY NOT per-chunk files (rejected, Section 8): a mature save touches tens of
 * thousands of chunks. On XP/NTFS each tiny file burns >= one 4 KiB cluster +
 * an ~1 KiB MFT record, so a ~400-byte compressed chunk wastes ~4.6 KiB to
 * metadata + slack (the overhead DWARFS the data); the MFT fragments; directory
 * enumeration (which streaming does constantly as the player moves) degrades
 * sharply past a few thousand entries; and one CreateFile/CloseHandle per
 * chunk, on a single core inside a 33.33 ms frame, is unabsorbable latency.
 *
 * WHY 32x32 horizontal, and why FULL vertical column (the file-count vs
 * index-size defence the brief asks for):
 *   - File count is BOUNDED and TINY. The loaded window is radius-6 horizontal
 *     this milestone (radius-12 shipping), so the player straddles at most a
 *     2x2 block of region files at once. Streaming therefore holds a working
 *     set of only ~4 (worst case ~9 across a diagonal) OPEN region handles,
 *     each opened ONCE and held, with chunks read/written by SEEKING inside the
 *     file - a handful of persistent handles, never thousands of transient
 *     ones. THIS is the entire point of regioning on XP.
 *   - Index size stays in one cheap read. 16,384 slots x 8 B = 128 KiB index =
 *     exactly 32 sectors; the whole index loads in one fread and lives resident
 *     while the handle is open. A larger region (64x64=64Ki slots -> 512 KiB
 *     index) would bloat that per-open read 4x for files that may hold only a
 *     few modified chunks; a smaller region (16x16) would multiply the open
 *     file count 4x and shrink the win. 32x32 is the Minecraft-proven sweet
 *     spot and the index still fits in L2 alongside the header.
 *   - FULL 16-layer vertical column per region (unlike a flat 2D region)
 *     mirrors the in-RAM "full vertical column resident" decision and keeps a
 *     player's whole base - which spans the vertical as they dig down / build
 *     up - inside ONE file, so saving a base is one file's worth of I/O.
 *
 * The index IS the modified-or-not bitmap (Section 8): an all-zero index entry
 * (sector_count == 0) means "never modified -> regenerate from seed", costing
 * no heap bytes and no sentinel record. */
#define REGION_CHUNKS_X    32u    /* chunk columns per region, X (power of two) */
#define REGION_CHUNKS_Z    32u    /* chunk columns per region, Z (power of two) */
#define REGION_CHUNKS_Y    16u    /* FULL vertical column (binding)             */
#define REGION_CHUNK_COUNT (REGION_CHUNKS_X * REGION_CHUNKS_Z * REGION_CHUNKS_Y) /* 16384 */

/* Sector size = the NTFS cluster (Section 8): records are allocated in whole
 * 4 KiB sectors so writes align to cluster boundaries (no read-modify-write of
 * a partial cluster). All file offsets below are in SECTOR units. */
#define REGION_SECTOR      4096u

/* "RVK1" little-endian: Region, VoXel, version 1. Bytes 'R','V','K','1' read
 * low-to-high on x86, so the magic also flags a wrong-endian reader. */
#define REGION_MAGIC       0x52564B31u   /* 'R'|'V'<<8|'K'<<16|'1'<<24          */

/* ---- chunk coord -> (region file, slot in that region) -------------------- *
 * Region coordinate is a pure shift (Section 8 binding coordinate math); >>5
 * because REGION_CHUNKS_X/Z == 32 == 2^5. Arithmetic shift on a signed coord
 * floors toward negative infinity, so negative chunk coords map to the correct
 * (negative) region - r.-1.-1 etc. The file name is r.<rx>.<rz>.dat. */
static inline int32_t region_coord_x(int32_t cx) { return cx >> 5; }
static inline int32_t region_coord_z(int32_t cz) { return cz >> 5; }

/* The chunk's slot index in its region's 16384-entry index table. The low bits
 * of the (signed) coord, masked to the region's local extent, give a 0-based
 * local position regardless of sign (two's-complement & is the right modulo for
 * a power-of-two extent). Layout: ly outermost, then lz, then lx innermost -
 * so a vertical column of a base clusters in the index. */
static inline uint32_t region_slot(int32_t cx, int32_t cy, int32_t cz)
{
    uint32_t lx = (uint32_t)cx & (REGION_CHUNKS_X - 1u);   /* cx mod 32         */
    uint32_t lz = (uint32_t)cz & (REGION_CHUNKS_Z - 1u);   /* cz mod 32         */
    uint32_t ly = (uint32_t)cy & (REGION_CHUNKS_Y - 1u);   /* cy mod 16 (0..15) */
    return (ly * REGION_CHUNKS_Z + lz) * REGION_CHUNKS_X + lx;
}

/* ======================================================================== *
 *  2. ON-DISK STRUCTS  (little-endian; written/read raw via fwrite/fread)   *
 * ======================================================================== *
 * File layout (Section 8):
 *   [ RegionHeader        : 64 B            ]
 *   [ RegionIndexEntry[16384] : 128 KiB     ]   <- one fread populates the index
 *   [ ... sector-aligned compressed chunk records (the record heap) ...     ]
 * Header + index = 64 B + 131072 B = 131136 B = 33 sectors (the heap starts at
 * sector 33). REGION_HEAP_START_SECTOR pins that so writer and reader agree. */

/* One index slot per POSSIBLE chunk in the region (16384 of them). An all-zero
 * entry (sector_count == 0) means "this chunk was never modified -> regenerate
 * from seed", and consumes no heap bytes. */
typedef struct {
    uint32_t sector_offset;  /* record start, in REGION_SECTOR units from file 0 */
    uint16_t sector_count;   /* sectors the record occupies; 0 == NOT STORED     */
    uint16_t compression;    /* PERSIST_COMPRESS_* selector for this record      */
} RegionIndexEntry;          /* 8 bytes - a single aligned write (crash-atomic)  */

typedef struct {
    uint32_t magic;          /* REGION_MAGIC; mismatch (or wrong endian) -> reject */
    uint32_t format_version; /* PERSIST_FORMAT_VERSION; newer -> reject (fwd-compat) */
    uint32_t gen_version;    /* WG_GEN_VERSION at write time; mismatch -> refuse  */
    uint32_t heap_sectors;   /* high-water mark of the record heap (next append)  */
    uint64_t world_seed;     /* this region belongs to THIS save; mismatch -> reject */
    int32_t  region_x;       /* region coords (sanity check vs the filename)      */
    int32_t  region_z;
    uint32_t game_version;   /* VOXEL_VERSION_PACKED at write time (0x00MMmmpp) -  *
                              * a Factorio-style save stamp for forward migration. *
                              * RECORDED, not rejected: a same-format save from a   *
                              * different game version still loads (only magic /    *
                              * format_version / gen_version / seed gate loading).  */
    uint8_t  reserved[28];   /* pad to 64 B; zero on write, ignored on read        */
} RegionHeader;              /* 64 bytes (4 + 28 == the old 32 B of padding)       */

/* Compile-time pins: the byte layout is part of the format contract, so assert
 * it rather than trust the compiler not to insert padding. (Both structs are
 * naturally aligned, so no packing pragma is needed on the x86 targets.) */
_Static_assert(sizeof(RegionIndexEntry) == 8,  "RegionIndexEntry must be 8 bytes");
_Static_assert(sizeof(RegionHeader)     == 64, "RegionHeader must be 64 bytes");

/* Derived fixed offsets (in sectors). The index immediately follows the header;
 * the heap immediately follows the index. ceil(133136 B / 4096) computed at
 * compile time without a runtime divide. */
#define REGION_INDEX_BYTES    (REGION_CHUNK_COUNT * 8u)               /* 131072 */
#define REGION_HEADER_INDEX_BYTES (64u + REGION_INDEX_BYTES)          /* 131136 */
#define REGION_HEAP_START_SECTOR  ((REGION_HEADER_INDEX_BYTES + REGION_SECTOR - 1u) / REGION_SECTOR) /* 33 */

/* ======================================================================== *
 *  3. PER-CHUNK COMPRESSED RECORD  (Section 8 "Palette + RLE")              *
 * ======================================================================== *
 * WHAT OF THE 4096-VOXEL ARRAY IS STORED (Section 2.1 state-vs-derived table,
 * binding). The 32-bit voxel word splits into three classes:
 *
 *   AUTHORED, MUST PERSIST:
 *     mat  (bits 0..7)   - material composition IS the player's creation. A
 *                          furnace/wall/ingot-pile is nothing but its voxel
 *                          materials; storing the array IS storing the structure.
 *     temp (bits 8..15)  - banked heat is a slow player-MANAGED resource; if it
 *                          reset to ambient on the eviction that fires the
 *                          instant the player walks ~100 m away, walking to a
 *                          far vein and back would ERASE their thermal work -
 *                          indistinguishable from a bug. Persisted. Compresses
 *                          superbly: heat is spatially smooth (long RLE runs)
 *                          and most of the world quantizes to a few ambient
 *                          codes (Section 8 defence).
 *     fill (bits 16..19) - fluid fill level; a half-full molten pool that reset
 *                          to "settled" would teleport liquid. Persisted.
 *
 *   DERIVED, NEVER PERSIST (Section 2.1: the only don't-care fields):
 *     light (bits 20..23), ao (bits 24..27) - pure mesh-time outputs recomputed
 *                          by light.c on the first mesh pass (zero extra traffic
 *                          - the mesher already touches the voxel). Storing them
 *                          would persist a stale CACHE. MASKED TO 0 before
 *                          palettizing -> better compression AND consistency.
 *
 *   TRANSIENT, NEVER PERSIST (Section 8 "freeze the fields, discard the index"):
 *     flags (bits 28..31) - VF_ACTIVE / VF_DIRTY_MESH are a transient
 *                          acceleration index, not world state; the CA RE-WAKES
 *                          any out-of-equilibrium voxel from the persisted
 *                          fields alone on load (more correct than restoring a
 *                          frozen wake-set, and storable-as-zero bytes). VF_LIQUID
 *                          is a flagged marginal call; Section 8's DEFAULT is
 *                          RE-DERIVE (persist NO flags, re-derive LIQUID from
 *                          mat+fill on the load-time CA re-scan). We take that
 *                          default: flags are masked to 0 too. So the persisted
 *                          per-voxel payload is exactly mat|temp|fill.
 *
 * THE CANONICALISATION MASK. Before palettizing, every voxel word is reduced to
 * its persistable bits by AND with PERSIST_VOX_MASK (= mat|temp|fill). This (a)
 * drops light/ao/flags so two voxels that differ ONLY in derived/transient bits
 * collapse to ONE palette entry and ONE long run, and (b) makes the format
 * independent of mesh/sim scratch state. On LOAD the canonical word is written
 * straight into voxels[] (light/ao/flags already 0); light.c rebakes light+ao
 * and the CA re-wakes from temp/fill - no post-processing needed. */
#define PERSIST_VOX_MASK   (VOX_MAT_MASK | VOX_TEMP_MASK | VOX_FILL_MASK) /* 0x000FFFFF */

/* Reduce a live voxel word to the bits we persist (Section 2.1). Light, AO and
 * flags fall away; what remains round-trips byte-exact. */
static inline Voxel persist_canon(Voxel v) { return v & PERSIST_VOX_MASK; }

/* Compression selector stored per record in RegionIndexEntry.compression and in
 * the record header. RLE-over-palette is the BINDING primary scheme (Section 8);
 * RAW is the worst-case escape hatch (see the bound below). A future optional
 * LZ4 wrap is reserved but deferred ("flagged as optional polish"). */
enum {
    PERSIST_COMPRESS_RLE_PALETTE = 1u,  /* palette + RLE over canonical words   */
    PERSIST_COMPRESS_RAW         = 2u   /* 4096 raw canonical u32 (escape hatch) */
    /* PERSIST_COMPRESS_RLE_LZ4 = 3u : reserved, deferred (Section 8 polish)    */
};

/* ---- THE TWO-STAGE COMPRESSION (Section 8) -------------------------------- *
 * Both stages run over the chunk's NATIVE linear order (voxel.h vox_index:
 * lx + ly*16 + lz*256 -> x INNERMOST/fastest, then y, then z OUTERMOST). That
 * IS Section 8's "z-major, then y, then x" traversal (z is the outer loop), so
 * save and load are a straight linear walk of voxels[] - no transpose. Vertical
 * strata and floor/ceiling layers, which dominate terrain, form long runs in
 * this order.
 *
 * STAGE 1 - PER-CHUNK PALETTE. Scan the 4096 CANONICAL words; collect the
 * DISTINCT ones into a palette (typically a handful: air, stone, dirt, one ore,
 * one fluid...). Each voxel is then a small PALETTE INDEX instead of a full
 * 32-bit word. index_bits is the minimum bits to address the palette: 1 if <=2
 * entries, 2 if <=4, 4 if <=16, 8 if <=256. If a pathological chunk exceeds
 * PERSIST_PALETTE_MAX distinct words, the palette is abandoned and the record
 * is written PERSIST_COMPRESS_RAW (bounded below).
 *
 * STAGE 2 - RLE OVER PALETTE INDICES. Walk the palette-indexed stream in linear
 * order emitting (count, palidx) runs: a homogeneous span of N identical
 * canonical words becomes ONE run. A uniform ambient stone stratum, an air void,
 * an untouched generated band all collapse to a single run. count is 1..4096.
 *
 * A mostly-uniform chunk: palette of ~4, a few dozen runs -> a few hundred bytes
 * to ~2 KiB stored, vs 16 KiB raw (8x-40x, Section 8). A fully-air regenerated-
 * but-flagged-modified chunk is ONE palette entry + ONE run -> tens of bytes. */
#define PERSIST_PALETTE_MAX   256u   /* >this distinct canon words -> store RAW  */

/* The record header that the index entry points at. Followed IN THE FILE by:
 *     Voxel    palette[palette_count]     (canonical u32 words, little-endian)
 *     PersistRun runs[run_count]          (RLE_PALETTE) | OR raw words (RAW)
 * For PERSIST_COMPRESS_RAW: palette_count/run_count are 0 and CHUNK_VOXELS raw
 * canonical Voxel words follow the header instead of palette+runs. */
typedef struct {
    uint32_t magic;          /* REGION_MAGIC again (per-record corruption guard) */
    uint16_t voxel_count;    /* always CHUNK_VOXELS (4096); cheap sanity check   */
    uint16_t palette_count;  /* distinct canonical words; 0 for RAW              */
    int32_t  cx, cy, cz;     /* chunk coords (cross-check vs the index slot)     */
    uint32_t run_count;      /* number of PersistRun entries; 0 for RAW          */
    uint8_t  index_bits;     /* bits/palette index: 1,2,4,8 (informational)      */
    uint8_t  compression;    /* PERSIST_COMPRESS_* (mirrors the index entry)     */
    uint8_t  reserved[2];    /* pad to 4-byte alignment; zero on write           */
} ChunkRecordHeader;         /* 28 bytes (4+2+2+4+4+4+4+1+1+2)                    */

_Static_assert(sizeof(ChunkRecordHeader) == 28, "ChunkRecordHeader must be 28 bytes");

/* One RLE run over the palette-index stream. We store the palette index (NOT
 * the 32-bit word) so a run is small and the per-voxel material/temp/fill tuple
 * is carried ONCE per distinct value in the palette, never per run. */
typedef struct {
    uint16_t count;    /* run length, 1..CHUNK_VOXELS (4096)                     */
    uint16_t palidx;   /* index into palette[]; only low index_bits significant  */
} PersistRun;          /* 4 bytes                                                */

_Static_assert(sizeof(PersistRun) == 4, "PersistRun must be 4 bytes");

/* ---- WORST-CASE BOUND (the brief asks to bound it and cap it) ------------- *
 * Pathological chunk = all 4096 voxels DISTINCT (no two canonical words equal).
 *   - Palette path degenerates: 4096-entry palette (16 KiB) + 4096 single-voxel
 *     runs (16 KiB) + header = ~32 KiB, i.e. DOUBLE the raw 16 KiB. RLE never
 *     helps when nothing repeats, so we DO NOT take that path.
 *   - Instead, when palette_count would exceed PERSIST_PALETTE_MAX (256) the
 *     writer emits PERSIST_COMPRESS_RAW: ChunkRecordHeader + CHUNK_VOXELS raw
 *     canonical u32 = 28 + 16384 = 16412 bytes. This is the HARD CAP on any
 *     single record: 5 sectors (5 * 4096 = 20480 >= 16412). RLE_PALETTE is only
 *     ever chosen when it is genuinely smaller, so a record NEVER exceeds the
 *     RAW size. (Real chunks - even heavily edited ones - palettize to a handful
 *     of materials and a few dozen runs; RAW is the safety floor, not the norm.) */
#define PERSIST_RECORD_MAX_BYTES   (28u + CHUNK_VOXELS * 4u)   /* 16412          */
#define PERSIST_RECORD_MAX_SECTORS ((PERSIST_RECORD_MAX_BYTES + REGION_SECTOR - 1u) / REGION_SECTOR) /* 5 */

/* ======================================================================== *
 *  4. CRASH-SAFE WRITE ORDERING  (Section 8, binding)                       *
 * ======================================================================== *
 * The one ordering rule a single-player game needs (no journal, rejected as
 * over-engineering): WRITE THE RECORD TO THE HEAP COMPLETELY, fflush it, THEN
 * write the chunk's RegionIndexEntry as a single aligned 8-byte write, THEN
 * flush the updated RegionHeader.heap_sectors high-water mark. A crash between
 * record-write and index-write leaves an ORPHANED heap region (reclaimed by the
 * next compaction) but a CONSISTENT index still pointing at the last good
 * record - the world loses at most the un-flushed edits since the last save,
 * NEVER a corrupt chunk. persist.c performs this ordering inside persist_save_
 * chunk(); it is documented here as the contract callers rely on.
 *
 * REWRITE / FREE-SPACE: when a chunk is re-saved and grows past its current
 * sector_count, the new record is APPENDED at the heap high-water mark and the
 * index repointed (the old location becomes a free hole); when it shrinks or
 * fits, it is overwritten in place. Holes are reclaimed by an occasional
 * compaction pass (rare, single-player, amortized) - persist_compact_region() is
 * reserved for that and may be a no-op in the first cut. */

/* ======================================================================== *
 *  5. PUBLIC API  (what world.c calls; pure C, temp-dir testable)          *
 * ======================================================================== *
 * Opaque store. Owns: the save directory path, the cache of OPEN region-file
 * handles + their resident headers/indices (a tiny LRU of <= a handful, per the
 * 2x2 straddle bound), the world seed, and the gen_version it stamps. world.c
 * holds ONE of these (or NULL = persistence disabled, today's ephemeral M7
 * behaviour). NOT thread-safe (single-threaded engine). */
/* Guarded so world.c (which includes both world.h and persist.h) does not get a
 * duplicate typedef - a C99 -Wpedantic constraint violation. See world.h. */
#ifndef PERSIST_STORE_TYPEDEF
#define PERSIST_STORE_TYPEDEF
typedef struct PersistStore PersistStore;
#endif

/* OPEN/CREATE a save rooted at directory `save_dir` (created if missing via the
 * #ifdef _WIN32 _mkdir / POSIX mkdir helper inside persist.c). `seed` is the
 * world seed stamped into every region header and CHECKED on every region open
 * (a seed mismatch -> that region is refused, not silently mixed into another
 * save). `gen_version` is WG_GEN_VERSION (worldgen.h), stamped on write and
 * compared on read (mismatch -> refuse to load, Section 8 default policy).
 * Returns a heap-allocated store, or NULL on failure (bad dir, OOM). The
 * matching teardown is persist_close(). Tests pass a unique temp dir. */
PersistStore *persist_open(const char *save_dir, uint64_t seed,
                           uint32_t gen_version);

/* FLUSH any buffered region headers/indices to disk and CLOSE every open
 * region handle, then free the store. Safe to call once. Call from
 * world_shutdown AFTER persist_flush() so all resident modified chunks are
 * written first. NULL store -> no-op. */
void persist_close(PersistStore *ps);

/* SAVE one modified chunk (the EVICT and SHUTDOWN hooks). Canonicalises each
 * voxel (persist_canon: mat|temp|fill, light/ao/flags dropped), builds the
 * per-chunk palette + RLE record, and writes it into the region file for
 * (c->cx,c->cy,c->cz) with the crash-safe record-then-index ordering above,
 * opening/creating that region file on demand. The CALLER guards this with
 * `if (c->flags & CHUNK_MODIFIED)` - persist.c does not re-check the flag, it
 * just persists whatever chunk it is handed (so a forced save is possible).
 * After a successful save the caller MAY clear CHUNK_MODIFIED (the chunk now
 * matches disk). Returns 0 on success, non-zero on I/O failure (the caller logs
 * and drops the slab anyway - a failed save must not wedge eviction). NULL
 * store -> 0 (no-op success: persistence disabled). */
int persist_save_chunk(PersistStore *ps, const Chunk *c);

/* LOAD one chunk (the world_insert LOAD hook). If a stored record exists for
 * (cx,cy,cz) in its region (index sector_count != 0) AND the region's
 * magic/format/gen_version/seed all validate, expands palette+RLE into c->voxels
 * (writing canonical mat|temp|fill; light|ao|flags left 0 for light.c/the CA to
 * rebake/re-wake), sets c->cx/cy/cz, and returns 1 (HIT - caller skips cb.gen).
 * If no record exists, the region file is absent, or any validation fails,
 * returns 0 (MISS - caller falls through to worldgen, exactly today's path) and
 * leaves c untouched. A version/seed/magic mismatch returns 0 here (treated as
 * "regenerate") OR, per the Section 8 refuse-to-load policy, persist_open should
 * have already rejected the region; the load path is defensive either way.
 * NULL store -> 0 (MISS). This function does NO meshing or lighting - the caller
 * lights+meshes the loaded chunk through the normal world_insert remesh queue. */
int persist_load_chunk(PersistStore *ps, Chunk *c, int cx, int cy, int cz);

/* FLUSH region headers + indices to disk WITHOUT closing handles (autosave / a
 * pre-shutdown sweep). world_shutdown calls this after iterating residents and
 * persist_save_chunk()-ing every CHUNK_MODIFIED one, so the index/header
 * high-water marks hit disk before the handles close. Returns 0 on success.
 * NULL store -> 0. */
int persist_flush(PersistStore *ps);

/* OPTIONAL maintenance: reclaim free holes left by in-place-grow rewrites by
 * rewriting a region's live records compactly (Section 8 "occasional compaction
 * pass on save"). Rare, single-player, amortized; MAY be a no-op in the first
 * cut. `rx,rz` select the region; returns 0 on success. Declared so the contract
 * is complete; world.c need not call it this milestone. */
int persist_compact_region(PersistStore *ps, int32_t rx, int32_t rz);

/* ======================================================================== *
 *  WORLD MANAGEMENT  (0.4: named worlds + a Minecraft-style save/load UI)   *
 * ======================================================================== *
 * A "world" is a save directory under a root (default "saves/") that holds the
 * region files PLUS a small text "world.meta" naming it and recording its seed.
 * The menu lists / creates / deletes worlds through these calls without knowing
 * the directory layout. Pure C99 stdio, plus ONE #ifdef for directory scanning
 * (opendir / FindFirstFile) - the only OS-specific addition, like persist_mkdir. */
#define WORLD_NAME_MAX  48
#define WORLD_DIR_MAX   96

typedef struct {
    char     dir[WORLD_DIR_MAX];    /* save directory, e.g. "saves/new-world"      */
    char     name[WORLD_NAME_MAX];  /* display name (from world.meta)              */
    uint64_t seed;                  /* the world's seed                            */
} WorldInfo;

/* Write <dir>/world.meta (creating <dir>); returns 0 on success. */
int  persist_world_meta_write(const char *dir, const char *name, uint64_t seed);

/* Read <dir>/world.meta into name_out[cap] + *seed_out; 0 on success, non-zero if
 * the dir has no readable/valid world.meta (so a non-world dir is skipped). */
int  persist_world_meta_read(const char *dir, char *name_out, int name_cap,
                             uint64_t *seed_out);

/* List worlds under `root` (subdirs with a valid world.meta) into out[max], sorted
 * by name; returns the count (<= max). */
int  persist_list_worlds(const char *root, WorldInfo *out, int max);

/* Create a new world under `root`: derive a unique directory from `name`, write
 * its world.meta(name, seed), and copy the directory into dir_out[cap]. 0 on ok. */
int  persist_world_create(const char *root, const char *name, uint64_t seed,
                          char *dir_out, int dir_cap);

/* Delete a world directory and the files in it (region files + world.meta). */
int  persist_world_delete(const char *dir);

#endif /* PERSIST_H */
