/* persist.c - World persistence: store the player's DELTA from the seed.
 *
 * Implements the contract in persist.h: region files (32 x 32 x 16 chunks each)
 * carrying a 64 B header + a 16384-entry index, with per-chunk PALETTE + RLE
 * records over the CANONICAL voxel word (mat|temp|fill; light/ao/flags masked to
 * 0 and rebaked on load). Only CHUNK_MODIFIED chunks are written; everything
 * else regenerates from seed (ARCHITECTURE.md Section 8 "Store Deltas,
 * Regenerate the Rest"). world.c calls persist_load_chunk on insert,
 * persist_save_chunk on evict, and persist_flush + persist_close on shutdown.
 *
 * PORTABILITY (the engine's FIRST disk I/O): pure C99 stdio - fopen("wb"/"rb"),
 * fread/fwrite, fseek/ftell - identical on the Linux dev host and the XP MinGW
 * target. The only OS-specific call is creating the save directory, hidden
 * behind the one-line #ifdef _WIN32 _mkdir / POSIX mkdir helper below.
 *
 * ENDIANNESS: dev box and XP target are BOTH little-endian x86, so on-disk
 * structs and raw u32 voxel words are written/read DIRECTLY with fwrite/fread,
 * no byte-swapping (the REGION_MAGIC + version stamp REFUSES a wrong-endian or
 * newer-format reader rather than silently mis-reading).
 *
 * SINGLE-THREADED: no locks, no async I/O. A handful of open region handles are
 * held resident (the 2x2-straddle bound) with their headers/indices cached in
 * RAM and seeked into; closing/eviction flushes them.
 *
 * No GL. Pure C99, temp-dir testable.
 */
#include "persist.h"
#include "version.h"   /* VOXEL_VERSION_PACKED - the save-file version stamp */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>     /* _mkdir, _rmdir */
#  include <windows.h>    /* FindFirstFile/FindNextFile/DeleteFile (world listing) */
#else
#  include <unistd.h>     /* rmdir */
#  include <dirent.h>     /* opendir/readdir (world listing) */
#endif

/* ======================================================================== *
 *  PRIVATE: portable mkdir                                                  *
 * ======================================================================== *
 * The one OS-specific call in the module (persist.h PORTABILITY note). XP
 * MinGW's _mkdir takes no mode; POSIX mkdir takes a mode. Returns 0 on success
 * OR when the directory already exists (EEXIST), so re-opening an existing save
 * is not an error; non-zero on any other failure. */
static int persist_mkdir_one(const char *path)
{
    int rc;
#ifdef _WIN32
    rc = _mkdir(path);
#else
    rc = mkdir(path, 0777);
#endif
    if (rc == 0)
        return 0;
    /* Already-exists is success for our purposes. Any errno value other than
     * EEXIST is a genuine failure (bad path, no permission, ...). We test the
     * directory rather than trust errno alone, since EEXIST is the only benign
     * case and a stat hides errno portability quirks. */
    {
        struct stat st;
        if (stat(path, &st) == 0)
            return 0;       /* the path exists - treat as success */
    }
    return -1;
}

/* Create `path` AND every missing parent component ("mkdir -p"). The default
 * save dir is two levels deep ("saves/<seed>"); a single mkdir of the leaf
 * FAILS with ENOENT when the parent "saves/" does not exist yet, which silently
 * disabled persistence on a fresh checkout (the store stayed NULL and every edit
 * was ephemeral). We walk the path, creating each prefix at every separator, then
 * the leaf. Both '/' and (on Windows) '\\' separate; a leading separator (POSIX
 * absolute path) or "C:" prefix is skipped as an empty/root component. Returns 0
 * on success (or already-exists), non-zero if any component cannot be created. */
static int persist_mkdir(const char *path)
{
    char buf[1024];
    size_t i, n = strlen(path);

    if (n == 0)
        return -1;
    if (n >= sizeof buf)                 /* too long to build prefixes safely */
        return persist_mkdir_one(path);  /* best effort: try the leaf as-is   */

    memcpy(buf, path, n + 1);            /* includes the NUL */
    for (i = 1; i < n; ++i) {           /* i=1: never mkdir "" for a leading / */
        char ch = buf[i];
        int sep = (ch == '/');
#ifdef _WIN32
        sep = sep || (ch == '\\');
#endif
        if (sep) {
            buf[i] = '\0';
            if (persist_mkdir_one(buf) != 0)
                return -1;              /* a parent could not be created */
            buf[i] = ch;
        }
    }
    return persist_mkdir_one(buf);       /* finally the leaf itself */
}

/* ======================================================================== *
 *  PRIVATE: open-region cache                                               *
 * ======================================================================== *
 * The store holds a tiny fixed-size cache of OPEN region handles, each with its
 * header + the full 16384-entry index resident in RAM. The loaded window
 * straddles at most a 2x2 (worst case diagonal 3x3) block of region files, so a
 * cache of PERSIST_REGION_CACHE entries holds the working set with one eviction
 * only on a long diagonal traverse. Each cached region keeps a dirty flag so a
 * header/index that changed since last flush is written back on flush/evict. */
#define PERSIST_REGION_CACHE 9   /* >= the 3x3 worst-case straddle */

typedef struct {
    FILE        *fp;             /* open region file handle, NULL == empty slot   */
    int32_t      rx, rz;         /* region coords this slot caches                */
    RegionHeader hdr;            /* resident copy of the 64 B header              */
    RegionIndexEntry *index;     /* resident 16384-entry index (heap, 128 KiB)    */
    int          dirty;          /* header/index changed since last flush         */
    uint32_t     lru;            /* last-use tick for cache replacement           */
} RegionSlot;

struct PersistStore {
    char       *dir;             /* save directory path (owned copy)              */
    uint64_t    seed;            /* world seed stamped/checked in every header    */
    uint32_t    gen_version;     /* WG_GEN_VERSION stamped/checked on read        */
    RegionSlot  cache[PERSIST_REGION_CACHE];
    uint32_t    lru_clock;       /* monotonically increasing use counter          */
};

/* ======================================================================== *
 *  PRIVATE: region file path                                                *
 * ======================================================================== *
 * "<dir>/r.<rx>.<rz>.dat" (persist.h coordinate math). snprintf bounds the
 * write; returns 0 on success, -1 if the path would truncate. */
static int region_path(const PersistStore *ps, int32_t rx, int32_t rz,
                       char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/r.%ld.%ld.dat",
                     ps->dir, (long)rx, (long)rz);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

/* ======================================================================== *
 *  PRIVATE: header/index I/O                                                *
 * ======================================================================== */

/* Write a slot's resident header + full index to its file and clear its dirty
 * flag. Header at offset 0, index immediately after (offset 64). fflush so the
 * bytes hit the OS before the caller proceeds. Returns 0 on success. */
static int region_write_meta(RegionSlot *s)
{
    if (!s->fp)
        return -1;
    if (fseek(s->fp, 0L, SEEK_SET) != 0)
        return -1;
    if (fwrite(&s->hdr, sizeof s->hdr, 1, s->fp) != 1)
        return -1;
    if (fwrite(s->index, sizeof(RegionIndexEntry), REGION_CHUNK_COUNT, s->fp)
            != (size_t)REGION_CHUNK_COUNT)
        return -1;
    if (fflush(s->fp) != 0)
        return -1;
    s->dirty = 0;
    return 0;
}

/* Initialise a brand-new region file: stamp the header, zero the index (all
 * sector_count == 0 == "no chunk stored -> regenerate"), and write both. The
 * heap starts at REGION_HEAP_START_SECTOR with nothing in it yet. */
static int region_init_new(RegionSlot *s, const PersistStore *ps,
                           int32_t rx, int32_t rz)
{
    memset(&s->hdr, 0, sizeof s->hdr);
    s->hdr.magic          = REGION_MAGIC;
    s->hdr.format_version = PERSIST_FORMAT_VERSION;
    s->hdr.gen_version    = ps->gen_version;
    s->hdr.heap_sectors   = REGION_HEAP_START_SECTOR; /* heap empty: HWM == start */
    s->hdr.world_seed     = ps->seed;
    s->hdr.region_x       = rx;
    s->hdr.region_z       = rz;
    s->hdr.game_version   = VOXEL_VERSION_PACKED; /* Factorio-style save stamp */

    memset(s->index, 0, REGION_INDEX_BYTES);
    s->dirty = 1;
    return region_write_meta(s);
}

/* Read + validate an existing region file's header and load its index into the
 * slot. Returns 0 on success (valid, matching), non-zero on any mismatch or I/O
 * failure (the caller treats a validation failure as "regenerate", per the
 * Section 8 defensive load policy). */
static int region_read_meta(RegionSlot *s, const PersistStore *ps,
                            int32_t rx, int32_t rz)
{
    if (fseek(s->fp, 0L, SEEK_SET) != 0)
        return -1;
    if (fread(&s->hdr, sizeof s->hdr, 1, s->fp) != 1)
        return -1;

    /* Validate the stamp before trusting the layout. A wrong-endian reader sees
     * the magic byte-swapped and is refused here; a newer format is refused
     * (forward-compat gate); a generator or seed mismatch is refused (Section 8
     * refuse-to-load default). */
    if (s->hdr.magic != REGION_MAGIC)
        return -1;
    if (s->hdr.format_version != PERSIST_FORMAT_VERSION)
        return -1;
    if (s->hdr.gen_version != ps->gen_version)
        return -1;
    if (s->hdr.world_seed != ps->seed)
        return -1;
    if (s->hdr.region_x != rx || s->hdr.region_z != rz)
        return -1;

    if (fread(s->index, sizeof(RegionIndexEntry), REGION_CHUNK_COUNT, s->fp)
            != (size_t)REGION_CHUNK_COUNT)
        return -1;

    s->dirty = 0;
    return 0;
}

/* ======================================================================== *
 *  PRIVATE: cache management                                                *
 * ======================================================================== */

/* Close a cache slot, writing back its meta first if dirty. Frees the resident
 * index. Leaves the slot empty (fp == NULL). */
static void slot_close(RegionSlot *s)
{
    if (!s->fp)
        return;
    if (s->dirty)
        (void)region_write_meta(s);   /* best-effort flush on close */
    fclose(s->fp);
    s->fp = NULL;
    free(s->index);
    s->index = NULL;
    s->dirty = 0;
}

/* Find the cache slot for (rx,rz) if resident; return NULL if not cached. */
static RegionSlot *slot_find(PersistStore *ps, int32_t rx, int32_t rz)
{
    int i;
    for (i = 0; i < PERSIST_REGION_CACHE; ++i) {
        RegionSlot *s = &ps->cache[i];
        if (s->fp && s->rx == rx && s->rz == rz)
            return s;
    }
    return NULL;
}

/* Pick a cache slot to (re)use for a new region: prefer an empty slot, else
 * evict the least-recently-used resident one (flushing it first). Always
 * returns a usable, empty (fp == NULL) slot. */
static RegionSlot *slot_evict_lru(PersistStore *ps)
{
    int i, victim = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (i = 0; i < PERSIST_REGION_CACHE; ++i) {
        if (!ps->cache[i].fp)
            return &ps->cache[i];       /* empty slot - use it */
        if (ps->cache[i].lru < oldest) {
            oldest = ps->cache[i].lru;
            victim = i;
        }
    }
    slot_close(&ps->cache[victim]);
    return &ps->cache[victim];
}

/* Get the cache slot for region (rx,rz), opening or creating the region file on
 * demand. `create` requests file creation when absent (the SAVE path); a LOAD
 * path passes create == 0 and gets NULL when the file does not exist (a MISS).
 * Returns the resident slot, or NULL on failure / absent-and-not-creating. */
static RegionSlot *region_get(PersistStore *ps, int32_t rx, int32_t rz,
                              int create)
{
    char path[1024];
    RegionSlot *s;
    FILE *fp;
    int is_new = 0;

    s = slot_find(ps, rx, rz);
    if (s) {
        s->lru = ++ps->lru_clock;
        return s;
    }

    if (region_path(ps, rx, rz, path, sizeof path) != 0)
        return NULL;

    /* Try to open an existing file for read+update. "rb+" does not create. */
    fp = fopen(path, "rb+");
    if (!fp) {
        if (!create)
            return NULL;            /* LOAD miss: no such region file */
        /* Create it: "wb+" truncates/creates, then we stamp a fresh header. */
        fp = fopen(path, "wb+");
        if (!fp)
            return NULL;
        is_new = 1;
    }

    s = slot_evict_lru(ps);
    s->fp    = fp;
    s->rx    = rx;
    s->rz    = rz;
    s->dirty = 0;
    s->lru   = ++ps->lru_clock;
    s->index = (RegionIndexEntry *)malloc(REGION_INDEX_BYTES);
    if (!s->index) {
        fclose(fp);
        s->fp = NULL;
        return NULL;
    }

    if (is_new) {
        if (region_init_new(s, ps, rx, rz) != 0) {
            slot_close(s);
            return NULL;
        }
    } else {
        if (region_read_meta(s, ps, rx, rz) != 0) {
            /* Existing file is invalid/foreign (magic/version/seed mismatch or
             * truncated). Per Section 8, do not mix it into this save: on the
             * SAVE path re-initialise it (the foreign data is overwritten); on
             * the LOAD path treat as a miss. */
            if (create) {
                if (region_init_new(s, ps, rx, rz) != 0) {
                    slot_close(s);
                    return NULL;
                }
            } else {
                slot_close(s);
                return NULL;
            }
        }
    }
    return s;
}

/* ======================================================================== *
 *  PRIVATE: palette + RLE codec                                             *
 * ======================================================================== */

/* Minimum bits to address `n` palette entries: 1 (<=2), 2 (<=4), 4 (<=16),
 * 8 (<=256). Informational in the record header; the run stream stores full
 * 16-bit palette indices regardless (PersistRun.palidx), so this is metadata. */
static uint8_t palette_index_bits(uint32_t n)
{
    if (n <= 2u)  return 1u;
    if (n <= 4u)  return 2u;
    if (n <= 16u) return 4u;
    return 8u;
}

/* Encode chunk c into a freshly malloc'd record buffer (header + palette + runs,
 * OR header + raw canonical words). Sets *out_buf / *out_len; the caller frees
 * *out_buf. Returns 0 on success, non-zero on OOM.
 *
 * The two stages run over the chunk's NATIVE linear order (vox_index: x-fastest,
 * then y, then z), exactly the save/load linear walk persist.h specifies - no
 * transpose. Voxels are CANONICALISED (persist_canon: mat|temp|fill) so light/
 * ao/flags collapse and runs lengthen. */
static int record_encode(const Chunk *c, uint8_t **out_buf, size_t *out_len)
{
    /* Stage 1: build the palette of distinct canonical words. CHUNK_VOXELS is
     * 4096 so a linear scan with a small dedup list is fine; we cap at
     * PERSIST_PALETTE_MAX (256) distinct words, falling back to RAW above that.
     * The palette can in the worst (pathological) case need 4096 entries before
     * we know to bail, so size it for the cap+1 probe. */
    Voxel palette[PERSIST_PALETTE_MAX];
    uint16_t pal_count = 0;
    uint16_t indices[CHUNK_VOXELS];   /* per-voxel palette index */
    int use_raw = 0;
    int i;

    for (i = 0; i < CHUNK_VOXELS; ++i) {
        Voxel cw = persist_canon(chunk_vox(c, i));
        uint16_t pi;
        int found = -1;
        uint16_t j;
        for (j = 0; j < pal_count; ++j) {
            if (palette[j] == cw) { found = (int)j; break; }
        }
        if (found >= 0) {
            pi = (uint16_t)found;
        } else {
            if (pal_count >= PERSIST_PALETTE_MAX) {
                use_raw = 1;          /* too many distinct words -> RAW */
                break;
            }
            palette[pal_count] = cw;
            pi = pal_count;
            ++pal_count;
        }
        indices[i] = pi;
    }

    if (use_raw) {
        /* RAW escape hatch (persist.h worst-case bound): header + CHUNK_VOXELS
         * raw canonical u32 words. palette_count / run_count are 0. */
        size_t len = sizeof(ChunkRecordHeader) + (size_t)CHUNK_VOXELS * sizeof(Voxel);
        uint8_t *buf = (uint8_t *)malloc(len);
        ChunkRecordHeader hdr;
        Voxel *words;
        if (!buf)
            return -1;
        memset(&hdr, 0, sizeof hdr);
        hdr.magic         = REGION_MAGIC;
        hdr.voxel_count   = (uint16_t)CHUNK_VOXELS;
        hdr.palette_count = 0;
        hdr.cx = c->cx; hdr.cy = c->cy; hdr.cz = c->cz;
        hdr.run_count     = 0;
        hdr.index_bits    = 0;
        hdr.compression   = (uint8_t)PERSIST_COMPRESS_RAW;
        memcpy(buf, &hdr, sizeof hdr);
        words = (Voxel *)(buf + sizeof hdr);
        for (i = 0; i < CHUNK_VOXELS; ++i)
            words[i] = persist_canon(chunk_vox(c, i));
        *out_buf = buf;
        *out_len = len;
        return 0;
    }

    /* Stage 2: RLE over the palette-index stream. Count runs first to size the
     * buffer exactly, then emit. A run is a maximal span of equal indices, with
     * count capped at CHUNK_VOXELS (4096), which never splits here since one
     * chunk has exactly CHUNK_VOXELS voxels. */
    {
        uint32_t run_count = 0;
        int k;
        for (k = 0; k < CHUNK_VOXELS; ) {
            uint16_t v = indices[k];
            int run = 1;
            while (k + run < CHUNK_VOXELS && indices[k + run] == v)
                ++run;
            ++run_count;
            k += run;
        }

        {
            size_t len = sizeof(ChunkRecordHeader)
                       + (size_t)pal_count * sizeof(Voxel)
                       + (size_t)run_count * sizeof(PersistRun);
            uint8_t *buf;
            ChunkRecordHeader hdr;
            Voxel *pal_out;
            PersistRun *runs_out;
            uint32_t ri;

            /* If the palette path is not actually smaller than RAW (pathological
             * but possible when palette is large and runs short), fall back to
             * RAW so a record NEVER exceeds the RAW cap (persist.h bound). */
            size_t raw_len = sizeof(ChunkRecordHeader)
                           + (size_t)CHUNK_VOXELS * sizeof(Voxel);
            if (len >= raw_len) {
                uint8_t *rbuf = (uint8_t *)malloc(raw_len);
                Voxel *words;
                if (!rbuf)
                    return -1;
                memset(&hdr, 0, sizeof hdr);
                hdr.magic         = REGION_MAGIC;
                hdr.voxel_count   = (uint16_t)CHUNK_VOXELS;
                hdr.palette_count = 0;
                hdr.cx = c->cx; hdr.cy = c->cy; hdr.cz = c->cz;
                hdr.run_count     = 0;
                hdr.index_bits    = 0;
                hdr.compression   = (uint8_t)PERSIST_COMPRESS_RAW;
                memcpy(rbuf, &hdr, sizeof hdr);
                words = (Voxel *)(rbuf + sizeof hdr);
                for (i = 0; i < CHUNK_VOXELS; ++i)
                    words[i] = persist_canon(chunk_vox(c, i));
                *out_buf = rbuf;
                *out_len = raw_len;
                return 0;
            }

            buf = (uint8_t *)malloc(len);
            if (!buf)
                return -1;

            memset(&hdr, 0, sizeof hdr);
            hdr.magic         = REGION_MAGIC;
            hdr.voxel_count   = (uint16_t)CHUNK_VOXELS;
            hdr.palette_count = pal_count;
            hdr.cx = c->cx; hdr.cy = c->cy; hdr.cz = c->cz;
            hdr.run_count     = run_count;
            hdr.index_bits    = palette_index_bits(pal_count);
            hdr.compression   = (uint8_t)PERSIST_COMPRESS_RLE_PALETTE;
            memcpy(buf, &hdr, sizeof hdr);

            pal_out  = (Voxel *)(buf + sizeof hdr);
            memcpy(pal_out, palette, (size_t)pal_count * sizeof(Voxel));

            runs_out = (PersistRun *)(buf + sizeof hdr
                                      + (size_t)pal_count * sizeof(Voxel));
            ri = 0;
            for (k = 0; k < CHUNK_VOXELS; ) {
                uint16_t v = indices[k];
                int run = 1;
                while (k + run < CHUNK_VOXELS && indices[k + run] == v)
                    ++run;
                runs_out[ri].count  = (uint16_t)run;
                runs_out[ri].palidx = v;
                ++ri;
                k += run;
            }

            *out_buf = buf;
            *out_len = len;
            return 0;
        }
    }
}

/* Decode a record buffer of `len` bytes into c->voxels (canonical words written
 * straight in; light/ao/flags left 0 for light.c / the CA to rebake/re-wake).
 * Validates the embedded header (magic + voxel_count) and that the byte layout
 * fits inside `len`. Returns 0 on success, non-zero on any malformedness (the
 * caller treats that as a MISS -> regenerate). */
static int record_decode(Chunk *c, const uint8_t *buf, size_t len,
                         int cx, int cy, int cz)
{
    ChunkRecordHeader hdr;
    int i;

    if (len < sizeof(ChunkRecordHeader))
        return -1;
    memcpy(&hdr, buf, sizeof hdr);

    if (hdr.magic != REGION_MAGIC)
        return -1;
    if (hdr.voxel_count != (uint16_t)CHUNK_VOXELS)
        return -1;
    /* Coords are a cross-check against the index slot the caller resolved. */
    if (hdr.cx != cx || hdr.cy != cy || hdr.cz != cz)
        return -1;

    if (hdr.compression == (uint8_t)PERSIST_COMPRESS_RAW) {
        size_t need = sizeof(ChunkRecordHeader)
                    + (size_t)CHUNK_VOXELS * sizeof(Voxel);
        const Voxel *words;
        if (len < need)
            return -1;
        words = (const Voxel *)(buf + sizeof(ChunkRecordHeader));
        for (i = 0; i < CHUNK_VOXELS; ++i)
            c->voxels[i] = persist_canon(words[i]);  /* canon: defensive */
    } else if (hdr.compression == (uint8_t)PERSIST_COMPRESS_RLE_PALETTE) {
        size_t need = sizeof(ChunkRecordHeader)
                    + (size_t)hdr.palette_count * sizeof(Voxel)
                    + (size_t)hdr.run_count * sizeof(PersistRun);
        const Voxel *pal;
        const PersistRun *runs;
        uint32_t r;
        int out = 0;
        if (hdr.palette_count == 0 || hdr.palette_count > PERSIST_PALETTE_MAX)
            return -1;
        if (len < need)
            return -1;
        pal  = (const Voxel *)(buf + sizeof(ChunkRecordHeader));
        runs = (const PersistRun *)(buf + sizeof(ChunkRecordHeader)
                                    + (size_t)hdr.palette_count * sizeof(Voxel));
        for (r = 0; r < hdr.run_count; ++r) {
            uint16_t cnt = runs[r].count;
            uint16_t pi  = runs[r].palidx;
            Voxel cw;
            int n;
            if (pi >= hdr.palette_count)
                return -1;
            if (cnt == 0 || out + (int)cnt > CHUNK_VOXELS)
                return -1;
            cw = persist_canon(pal[pi]);
            for (n = 0; n < (int)cnt; ++n)
                c->voxels[out++] = cw;
        }
        if (out != CHUNK_VOXELS)        /* runs must cover the whole chunk */
            return -1;
    } else {
        return -1;                       /* unknown compression scheme */
    }

    c->cx = cx;
    c->cy = cy;
    c->cz = cz;
    return 0;
}

/* ======================================================================== *
 *  PUBLIC API                                                               *
 * ======================================================================== */

PersistStore *persist_open(const char *save_dir, uint64_t seed,
                           uint32_t gen_version)
{
    PersistStore *ps;
    size_t dirlen;

    if (!save_dir)
        return NULL;

    if (persist_mkdir(save_dir) != 0)
        return NULL;

    ps = (PersistStore *)calloc(1, sizeof *ps);
    if (!ps)
        return NULL;

    dirlen = strlen(save_dir);
    ps->dir = (char *)malloc(dirlen + 1);
    if (!ps->dir) {
        free(ps);
        return NULL;
    }
    memcpy(ps->dir, save_dir, dirlen + 1);

    ps->seed        = seed;
    ps->gen_version = gen_version;
    ps->lru_clock   = 0u;
    /* calloc zeroed cache[]: every slot has fp == NULL, index == NULL. */
    return ps;
}

void persist_close(PersistStore *ps)
{
    int i;
    if (!ps)
        return;
    for (i = 0; i < PERSIST_REGION_CACHE; ++i)
        slot_close(&ps->cache[i]);
    free(ps->dir);
    free(ps);
}

int persist_save_chunk(PersistStore *ps, const Chunk *c)
{
    int32_t rx, rz;
    uint32_t slot;
    RegionSlot *s;
    uint8_t *rec = NULL;
    size_t rec_len = 0;
    uint16_t sectors;
    uint32_t sector_off;
    RegionIndexEntry *ent;

    if (!ps || !c)
        return 0;                        /* persistence disabled / nothing to do */

    rx   = region_coord_x(c->cx);
    rz   = region_coord_z(c->cz);
    slot = region_slot(c->cx, c->cy, c->cz);

    s = region_get(ps, rx, rz, 1 /* create */);
    if (!s)
        return -1;

    if (record_encode(c, &rec, &rec_len) != 0)
        return -1;

    /* Sectors this record occupies (ceil to REGION_SECTOR). Bounded by
     * PERSIST_RECORD_MAX_SECTORS == 5 (persist.h hard cap). */
    sectors = (uint16_t)((rec_len + REGION_SECTOR - 1u) / REGION_SECTOR);

    ent = &s->index[slot];

    /* REWRITE policy (persist.h free-space note): overwrite in place when the
     * record still fits the chunk's existing allocation; otherwise APPEND at the
     * heap high-water mark (the old location becomes a free hole reclaimed by a
     * future compaction). A first save (sector_count == 0) always appends. */
    if (ent->sector_count >= sectors && ent->sector_offset >= REGION_HEAP_START_SECTOR) {
        sector_off = ent->sector_offset;            /* fits in place */
    } else {
        sector_off = s->hdr.heap_sectors;           /* append at HWM */
        s->hdr.heap_sectors += sectors;
    }

    /* CRASH-SAFE ORDERING (persist.h Section 4): write the record to the heap
     * and fflush it FIRST, then write the 8-byte index entry, then flush the
     * header high-water mark. A crash between record and index leaves an
     * orphaned heap region but a consistent index. */
    if (fseek(s->fp, (long)sector_off * (long)REGION_SECTOR, SEEK_SET) != 0) {
        free(rec);
        return -1;
    }
    if (fwrite(rec, 1, rec_len, s->fp) != rec_len) {
        free(rec);
        return -1;
    }
    free(rec);
    if (fflush(s->fp) != 0)
        return -1;

    /* Commit the index entry (single aligned 8-byte write at its slot offset),
     * then persist the header (heap HWM). region_write_meta rewrites the whole
     * header+index in one pass - cheap (33 sectors) and keeps the resident copy
     * authoritative. */
    ent->sector_offset = sector_off;
    ent->sector_count  = sectors;
    ent->compression   = (uint16_t)(rec_len >= (sizeof(ChunkRecordHeader)
                                     + (size_t)CHUNK_VOXELS * sizeof(Voxel))
                                    ? PERSIST_COMPRESS_RAW
                                    : PERSIST_COMPRESS_RLE_PALETTE);
    s->dirty = 1;

    if (region_write_meta(s) != 0)
        return -1;

    return 0;
}

int persist_load_chunk(PersistStore *ps, Chunk *c, int cx, int cy, int cz)
{
    int32_t rx, rz;
    uint32_t slot;
    RegionSlot *s;
    RegionIndexEntry *ent;
    uint8_t *rec;
    size_t rec_len;

    if (!ps || !c)
        return 0;                        /* persistence disabled -> MISS */

    rx   = region_coord_x(cx);
    rz   = region_coord_z(cz);
    slot = region_slot(cx, cy, cz);

    s = region_get(ps, rx, rz, 0 /* do not create */);
    if (!s)
        return 0;                        /* no region file -> MISS (regenerate) */

    ent = &s->index[slot];
    if (ent->sector_count == 0)
        return 0;                        /* never modified -> MISS (regenerate) */

    rec_len = (size_t)ent->sector_count * REGION_SECTOR;
    rec = (uint8_t *)malloc(rec_len);
    if (!rec)
        return 0;                        /* OOM -> treat as MISS, do not wedge */

    if (fseek(s->fp, (long)ent->sector_offset * (long)REGION_SECTOR, SEEK_SET) != 0) {
        free(rec);
        return 0;
    }
    /* The record may be shorter than sector_count*SECTOR (slack to the cluster
     * boundary); a short fread is fine as long as the header+payload arrived.
     * record_decode validates lengths internally, so read what is there. */
    {
        size_t got = fread(rec, 1, rec_len, s->fp);
        if (got < sizeof(ChunkRecordHeader)) {
            free(rec);
            return 0;
        }
        if (record_decode(c, rec, got, cx, cy, cz) != 0) {
            free(rec);
            return 0;                    /* malformed -> MISS (regenerate) */
        }
    }
    free(rec);
    return 1;                            /* HIT - caller skips worldgen */
}

int persist_flush(PersistStore *ps)
{
    int i, rc = 0;
    if (!ps)
        return 0;
    for (i = 0; i < PERSIST_REGION_CACHE; ++i) {
        RegionSlot *s = &ps->cache[i];
        if (s->fp && s->dirty) {
            if (region_write_meta(s) != 0)
                rc = -1;
        }
    }
    return rc;
}

int persist_compact_region(PersistStore *ps, int32_t rx, int32_t rz)
{
    /* OPTIONAL maintenance (persist.h Section 4 reserved). In-place-grow
     * rewrites leave free holes; a compaction pass would rewrite a region's live
     * records contiguously. Single-player, rare, amortized - deferred for this
     * milestone. Declared so the contract is complete; world.c need not call it.
     * No-op success keeps callers correct. */
    (void)ps; (void)rx; (void)rz;
    return 0;
}

/* ======================================================================== *
 *  WORLD MANAGEMENT  (0.4 named worlds + save/load UI)                      *
 * ======================================================================== */

int persist_world_meta_write(const char *dir, const char *name, uint64_t seed)
{
    char path[512];
    FILE *f;
    if (dir == NULL || persist_mkdir(dir) != 0)
        return -1;
    snprintf(path, sizeof path, "%s/world.meta", dir);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    fprintf(f, "VOXELWORLD 1\n%016llx\n%s\n",
            (unsigned long long)seed, (name != NULL && name[0]) ? name : "world");
    fclose(f);
    return 0;
}

int persist_world_meta_read(const char *dir, char *name_out, int name_cap, uint64_t *seed_out)
{
    char path[512], line[256];
    FILE *f;
    unsigned long long s = 0;
    if (dir == NULL)
        return -1;
    snprintf(path, sizeof path, "%s/world.meta", dir);
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    if (fgets(line, sizeof line, f) == NULL || strncmp(line, "VOXELWORLD", 10) != 0) {
        fclose(f);
        return -1;
    }
    if (fgets(line, sizeof line, f) == NULL || sscanf(line, "%llx", &s) != 1) {
        fclose(f);
        return -1;
    }
    if (name_out != NULL && name_cap > 0) {
        int i = 0;
        if (fgets(line, sizeof line, f) != NULL) {
            int n = (int)strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            for (; i < n && i < name_cap - 1; ++i)
                name_out[i] = line[i];
        }
        name_out[i] = '\0';
        if (name_out[0] == '\0')
            snprintf(name_out, (size_t)name_cap, "world");
    }
    fclose(f);
    if (seed_out != NULL)
        *seed_out = (uint64_t)s;
    return 0;
}

/* Insertion-sort the world list by display name (n is small). */
static void world_sort(WorldInfo *w, int n)
{
    int i, j;
    for (i = 1; i < n; ++i) {
        WorldInfo k = w[i];
        for (j = i - 1; j >= 0 && strcmp(w[j].name, k.name) > 0; --j)
            w[j + 1] = w[j];
        w[j + 1] = k;
    }
}

/* Record one candidate subdir if it carries a valid world.meta. The path is
 * built with length-checked memcpy (not snprintf) so the bounded-but-large
 * dirent name can never trip -Wformat-truncation; an over-long path (longer than
 * any slug we create) is simply skipped - it cannot be one of our worlds. */
static void world_try_add(const char *root, const char *leaf,
                          WorldInfo *out, int max, int *pn)
{
    char d[WORLD_DIR_MAX];
    char nm[WORLD_NAME_MAX];
    uint64_t sd;
    size_t rl, ll;
    if (*pn >= max || leaf[0] == '.')
        return;
    rl = strlen(root);
    ll = strlen(leaf);
    if (rl + 1u + ll >= sizeof d)                   /* too long to be ours */
        return;
    memcpy(d, root, rl);
    d[rl] = '/';
    memcpy(d + rl + 1, leaf, ll + 1);               /* + NUL */
    if (persist_world_meta_read(d, nm, WORLD_NAME_MAX, &sd) != 0)
        return;                                     /* not a world (skip) */
    memcpy(out[*pn].dir, d, rl + 1 + ll + 1);       /* fits (checked above) */
    snprintf(out[*pn].name, WORLD_NAME_MAX, "%s", nm);
    out[*pn].seed = sd;
    ++(*pn);
}

int persist_list_worlds(const char *root, WorldInfo *out, int max)
{
    int n = 0;
    if (root == NULL || out == NULL || max <= 0)
        return 0;
#ifdef _WIN32
    {
        WIN32_FIND_DATA fd;
        char pat[512];
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", root);
        h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    world_try_add(root, fd.cFileName, out, max, &n);
            } while (n < max && FindNextFile(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR *dp = opendir(root);
        struct dirent *de;
        if (dp != NULL) {
            while (n < max && (de = readdir(dp)) != NULL)
                world_try_add(root, de->d_name, out, max, &n);  /* meta-read filters non-dirs */
            closedir(dp);
        }
    }
#endif
    world_sort(out, n);
    return n;
}

int persist_world_create(const char *root, const char *name, uint64_t seed,
                          char *dir_out, int dir_cap)
{
    char slug[WORLD_NAME_MAX];
    char dir[WORLD_DIR_MAX];
    int i, k = 0, suffix;
    if (root == NULL)
        return -1;
    /* Slug: lowercased [a-z0-9], any run of other chars collapses to one '-'. */
    for (i = 0; name != NULL && name[i] && k < (int)sizeof slug - 1; ++i) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            slug[k++] = ch;
        else if (k > 0 && slug[k - 1] != '-')
            slug[k++] = '-';
    }
    while (k > 0 && slug[k - 1] == '-') --k;        /* trim trailing '-' */
    slug[k] = '\0';
    if (k == 0)
        snprintf(slug, sizeof slug, "world");
    /* First free dir: root/slug, root/slug-2, ... (skip any with a world.meta). */
    for (suffix = 1; suffix < 1000; ++suffix) {
        char nm[WORLD_NAME_MAX];
        uint64_t sd;
        if (suffix == 1) snprintf(dir, sizeof dir, "%s/%s", root, slug);
        else             snprintf(dir, sizeof dir, "%s/%s-%d", root, slug, suffix);
        if (persist_world_meta_read(dir, nm, WORLD_NAME_MAX, &sd) != 0)
            break;                                  /* free */
    }
    if (persist_world_meta_write(dir, (name != NULL && name[0]) ? name : "world", seed) != 0)
        return -1;
    if (dir_out != NULL && dir_cap > 0)
        snprintf(dir_out, (size_t)dir_cap, "%s", dir);
    return 0;
}

int persist_world_delete(const char *dir)
{
    if (dir == NULL)
        return -1;
#ifdef _WIN32
    {
        WIN32_FIND_DATA fd;
        char pat[512];
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char p[640];
                if (fd.cFileName[0] == '.') continue;
                snprintf(p, sizeof p, "%s/%s", dir, fd.cFileName);
                DeleteFile(p);
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
        return _rmdir(dir) == 0 ? 0 : -1;
    }
#else
    {
        DIR *dp = opendir(dir);
        struct dirent *de;
        if (dp != NULL) {
            while ((de = readdir(dp)) != NULL) {
                char p[640];
                if (de->d_name[0] == '.') continue;
                snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
                remove(p);
            }
            closedir(dp);
        }
        return rmdir(dir) == 0 ? 0 : -1;
    }
#endif
}
