// fat_core.c -- context lifecycle, BPB validation, FAT12/16/32 decode,
// directory iteration and path lookup. No stdio, no color: errors are
// fat_result_t. Every region access goes through the sector cache over
// the fat_io_t backend -- no whole-image buffer is ever held.

#include "fat_internal.h"
#include <stdlib.h>
#include <string.h>

// internal helpers --------------------------------------------------------

// host-endian little-endian readers
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// ... and writers (the rd16/rd32 mirror)
static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Read-only public entry points take a const context, yet every read fills
// the sector cache. The cache is logically-mutable state (a mutable member
// in C++ terms) and the context object is never const at its definition,
// so stripping the qualifier is safe.
static fat_ctx_t* fat_ctx_mut(const fat_ctx_t* ctx) {
    return (fat_ctx_t*)ctx;
}

// sector cache ------------------------------------------------------------

// flush one dirty slot to the backend; the slot stays dirty on failure so
// a later fat_sync retries it
static fat_result_t cache_flush_slot(fat_ctx_t* ctx, fat_cache_slot_t* s) {
    if (!s->valid || !s->dirty)
        return FAT_OK;
    uint32_t bps = ctx->geo.bytes_per_sector;
    fat_result_t r = ctx->io->write(ctx->io, s->sector * bps, s->data, bps);
    if (r != FAT_OK)
        return r;
    s->dirty = false;
    return FAT_OK;
}

// flush every dirty slot (fat_sync; fat_close does this best-effort)
static fat_result_t cache_flush_all(fat_ctx_t* ctx) {
    for (int i = 0; i < FAT_CACHE_SLOTS; i++) {
        fat_result_t r = cache_flush_slot(ctx, &ctx->cache[i]);
        if (r != FAT_OK)
            return r;
    }
    return FAT_OK;
}

// bring `sector` into its direct-mapped slot, flushing a displaced dirty
// victim first (an early flush ahead of fat_sync is permitted). The slot
// state is only updated after the fill succeeds, so a failed read leaves
// the cache exactly as it was.
static fat_result_t cache_sector(fat_ctx_t* ctx, uint64_t sector,
                                 fat_cache_slot_t** out) {
    uint32_t bps = ctx->geo.bytes_per_sector;
    fat_cache_slot_t* s = &ctx->cache[sector % FAT_CACHE_SLOTS];
    if (!s->valid || s->sector != sector) {
        fat_result_t r = cache_flush_slot(ctx, s);
        if (r != FAT_OK)
            return r;
        if (ctx->io->read(ctx->io, sector * bps, s->data, bps) != FAT_OK)
            return FAT_ERR_IO;
        s->valid = true;
        s->dirty = false;
        s->sector = sector;
    }
    *out = s;
    return FAT_OK;
}

// one sector-at-a-time transfer loop behind both directions: the memcpy
// direction and the write-back dirty flag are the only differences
static fat_result_t io_transfer(fat_ctx_t* ctx, uint64_t offset, void* buf,
                                size_t len, bool to_cache) {
    if (ctx == NULL || (len > 0 && buf == NULL))
        return FAT_ERR_INVALID_ARG;
    if (len == 0)
        return FAT_OK;
    if (offset > (uint64_t)ctx->image_size ||
        (uint64_t)len > (uint64_t)ctx->image_size - offset)
        return FAT_ERR_IO; // past the backend extent

    uint32_t bps = ctx->geo.bytes_per_sector;
    uint8_t* o = buf;
    uint64_t pos = offset;
    size_t left = len;
    while (left > 0) {
        fat_cache_slot_t* s;
        fat_result_t r = cache_sector(ctx, pos / bps, &s);
        if (r != FAT_OK)
            return r;
        size_t in_off = (size_t)(pos % bps);
        size_t n = (size_t)bps - in_off;
        if (n > left)
            n = left;
        if (to_cache) {
            memcpy(s->data + in_off, o, n); // write-back: dirty until flushed
            s->dirty = true;
        } else {
            memcpy(o, s->data + in_off, n);
        }
        o += n;
        pos += n;
        left -= n;
    }
    return FAT_OK;
}

fat_result_t fat_io_read(fat_ctx_t* ctx, uint64_t offset, void* buf,
                         size_t len) {
    return io_transfer(ctx, offset, buf, len, false);
}

fat_result_t fat_io_write(fat_ctx_t* ctx, uint64_t offset, const void* buf,
                          size_t len) {
    return io_transfer(ctx, offset, (void*)(uintptr_t)buf, len, true);
}

// FAT decode ---------------------------------------------------------------

// per-type FAT value facts (thresholds per the spec: FAT12 0xFF0/0xFF7/
// 0xFF8, FAT16 0xFFF0/0xFFF7/0xFFF8, FAT32 0x0FFFFFF0/0x0FFFFFF7/0x0FFFFFF8
// after masking). One table instead of parallel type switches -- the read
// side (classification), the write side (EOC constant, value ceiling) and
// the entry packing all hang off it.
typedef struct {
    uint32_t reserved_min; // >= ..< bad: never follow as a cluster number
    uint32_t bad;          // the bad-cluster marker value
    uint32_t eoc_min;      // >= is end-of-chain
    uint32_t eoc_const;    // canonical EOC value; also the storable maximum
} FatTypeInfo;

static const FatTypeInfo fat_type_info[] = {
    [FT_FAT12] = {0xFF0, 0xFF7, 0xFF8, 0xFFF},
    [FT_FAT16] = {0xFFF0, 0xFFF7, 0xFFF8, 0xFFFF},
    [FT_FAT32] = {0x0FFFFFF0, 0x0FFFFFF7, 0x0FFFFFF8, 0x0FFFFFFF},
};

// byte packing of FAT entry `cluster` (FAT12 packs two 12-bit entries into
// every 3 bytes): offset within one table and total bytes the entry's
// storage spans. Shared by the reader and the writer so the packing math
// lives in exactly one place.
static bool fat_entry_span(enum FAT_TYPE type, uint32_t cluster,
                           uint64_t* ent, size_t* need) {
    switch (type) {
    case FT_FAT16:
        *ent = (uint64_t)cluster * 2;
        *need = (size_t)cluster * 2 + 2;
        return true;
    case FT_FAT32:
        *ent = (uint64_t)cluster * 4;
        *need = (size_t)cluster * 4 + 4;
        return true;
    case FT_FAT12:
        *ent = (uint64_t)cluster * 3 / 2; // floor: the shared-byte pair start
        *need = (size_t)(cluster / 2) * 3 + 3;
        return true;
    default:
        return false;
    }
}

fat_result_t fat_raw_fat_entry(fat_ctx_t* ctx, uint32_t cluster,
                               uint32_t* out) {
    uint64_t ent; // entry offset within the active table
    size_t need;  // bytes of FAT the entry spans (bounds the index)
    if (!fat_entry_span(ctx->type, cluster, &ent, &need))
        return FAT_ERR_INVALID_BPB;
    size_t fat_bytes = (size_t)ctx->geo.fat_sectors *
                       ctx->geo.bytes_per_sector;
    if (need > fat_bytes)
        return FAT_ERR_INVALID_BPB; // FAT region does not cover the index

    uint64_t base = (uint64_t)ctx->geo.fat_start_sector *
                    ctx->geo.bytes_per_sector;
    uint8_t raw[4];
    fat_result_t r = fat_io_read(ctx, base + ent, raw,
                                 ctx->type == FT_FAT32 ? 4 : 2);
    if (r != FAT_OK)
        return r; // backend read failure

    switch (ctx->type) {
    case FT_FAT16:
        *out = rd16(raw);
        break;
    case FT_FAT32:
        // the upper 4 bits are reserved flags; masking is mandatory for
        // EOC detection
        *out = rd32(raw) & 0x0FFFFFFF;
        break;
    case FT_FAT12: {
        uint32_t value;
        if (cluster % 2 == 0) {
            value = raw[0];
            value |= (raw[1] & 0x0F) << 8;
        } else {
            value = (uint32_t)raw[1] << 4;
            value |= (raw[0] & 0xF0) >> 4;
        }
        *out = value;
        break;
    }
    default:
        return FAT_ERR_INVALID_BPB;
    }
    return FAT_OK;
}

// EOC / bad / reserved classification of a raw FAT value (the table lookup;
// FT_UNKNOWN can only appear on a NULL/half-built context, where every
// value refusing to classify is the safe answer)
static bool fat_is_bad(const fat_ctx_t* ctx, uint32_t entry) {
    return ctx->type == FT_UNKNOWN ||
           entry == fat_type_info[ctx->type].bad;
}

static bool fat_is_eoc(const fat_ctx_t* ctx, uint32_t entry) {
    return ctx->type != FT_UNKNOWN &&
           entry >= fat_type_info[ctx->type].eoc_min;
}

// reserved values (e.g. FAT12 0xFF0..0xFF6) must never be followed as
// cluster numbers
static bool fat_is_reserved(const fat_ctx_t* ctx, uint32_t entry) {
    return ctx->type == FT_UNKNOWN ||
           (entry >= fat_type_info[ctx->type].reserved_min &&
            entry < fat_type_info[ctx->type].bad);
}

// what one FAT link says about the next cluster of a chain -- the shared
// core of every chain guard. The bad-cluster marker is deliberately not
// classified here (its numeric range is type-dependent); callers that
// must reject it test fat_is_bad alongside.
typedef enum {
    CHAIN_END,  // EOC: the chain ends here (a healthy end)
    CHAIN_NEXT, // a followable cluster number; *next holds it
    CHAIN_BROKEN, // free/reserved value, or out of bounds: corrupt chain
} chain_link_t;

static chain_link_t chain_link(const fat_ctx_t* ctx, uint32_t fat,
                               uint32_t* next) {
    if (fat_is_eoc(ctx, fat))
        return CHAIN_END;
    if (fat == 0 || fat_is_reserved(ctx, fat) || fat < 2 ||
        fat >= 2 + ctx->geo.cluster_count)
        return CHAIN_BROKEN;
    *next = fat;
    return CHAIN_NEXT;
}

// region geometry ----------------------------------------------------------

// NTRes (dirent byte 12) lowercase flags: bit3 = fold the base to lowercase,
// bit4 = fold the extension. Creators of 8.3-reversible lowercase names
// store the uppercase 11 bytes plus these flags; folding them back is a
// rendering concern only (matching stays ASCII case-insensitive). Only
// ASCII 'A'..'Z' folds -- everything else, 0x05-escaped Kanji included,
// passes through untouched (§28.2).
static void nt_lower_apply(uint8_t ntres, char* name) {
    const bool lower_base = (ntres & 0x08) != 0;
    const bool lower_ext = (ntres & 0x10) != 0;
    bool in_ext = false;
    for (char* p = name; *p != '\0'; p++) {
        if (*p == '.')
            in_ext = true; // directories/labels carry no dot: base only
        else if (*p >= 'A' && *p <= 'Z' && (in_ext ? lower_ext : lower_base))
            *p = (char)(*p - 'A' + 'a');
    }
}

// FAT32 splits a dirent's first cluster over bytes 20/21 (high) and 26/27
// (low); FAT12/16 carry the low half only. Every site that decodes or
// encodes a raw slot's first cluster goes through this pair.
static uint32_t raw_first_cluster(enum FAT_TYPE type, const uint8_t* raw32) {
    uint32_t low = (uint32_t)raw32[26] | ((uint32_t)raw32[27] << 8);
    return type == FT_FAT32
               ? low | ((uint32_t)raw32[20] << 16) | ((uint32_t)raw32[21] << 24)
               : low;
}

static void raw_set_first_cluster(enum FAT_TYPE type, uint8_t* raw32,
                                  uint32_t cluster) {
    if (type == FT_FAT32) {
        raw32[20] = (uint8_t)(cluster >> 16); // FirstClusterHigh
        raw32[21] = (uint8_t)(cluster >> 24);
    }
    raw32[26] = (uint8_t)cluster; // FirstClusterLow
    raw32[27] = (uint8_t)(cluster >> 8);
}

// fill a public fat_dirent_t from 32 raw on-disk bytes
static void dirent_from_raw(const fat_ctx_t* ctx, const uint8_t* raw32,
                            fat_dirent_t* out) {
    const DirectoryEntry* e = (const DirectoryEntry*)raw32;
    fat_name_from_83(e->name, e->attributes, out->name, FAT_NAME_MAX);
    nt_lower_apply(e->reserved[0], out->name);
    out->attributes = e->attributes;
    out->creation_time_tenth = e->creationTimeTenthOfSecond;
    out->creation_time = e->creationTime;
    out->creation_date = e->creationDate;
    out->last_access_date = e->lastAccessDate;
    out->last_write_time = e->lastWriteTime;
    out->last_write_date = e->lastWriteDate;
    out->first_cluster = raw_first_cluster(ctx->type, raw32);
    out->file_size = e->fileSize;
}

// byte offset of data cluster `cluster`, or false when the whole cluster
// does not lie in the image. 64-bit intermediates: cluster *
// sectors_per_cluster * bytes_per_sector can exceed 32 bits in hostile
// BPBs.
static bool cluster_offset(const fat_ctx_t* ctx, uint32_t cluster,
                           uint64_t* out) {
    if (cluster < 2 || cluster >= 2 + ctx->geo.cluster_count)
        return false;
    uint64_t offset = ((uint64_t)ctx->geo.data_start_sector +
                       (uint64_t)(cluster - 2) * ctx->geo.sectors_per_cluster) *
                      ctx->geo.bytes_per_sector;
    if (offset + fat_cluster_size(ctx) > (uint64_t)ctx->image_size)
        return false;
    *out = offset;
    return true;
}

// context construction -------------------------------------------------------

fat_result_t fat_ctx_init(fat_io_t* io, fat_ctx_t** out) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    // close is the only optional vtable entry (fat.h); a NULL read/write/
    // size would crash later calls, so it is rejected up front
    if (io == NULL || io->read == NULL || io->write == NULL ||
        io->size == NULL)
        return FAT_ERR_INVALID_ARG;

    // Opening reads exactly one 512-byte sector: the boot sector. A backend
    // too small to serve it fails right here with FAT_ERR_IO -- a pre-size
    // check would misreport a short backend as an invalid BPB.
    uint8_t boot[512];
    if (io->read(io, 0, boot, sizeof(boot)) != FAT_OK)
        return FAT_ERR_IO;
    uint64_t backend_size = io->size(io);

    // validate the BPB before trusting any of its values
    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return FAT_ERR_INVALID_BPB;
    const FatBS* bs = (const FatBS*)boot;
    uint16_t bytesPerSector = rd16((const uint8_t*)&bs->bytesPerSector);
    uint8_t sectorsPerCluster = bs->sectorsPerCluster;
    uint16_t reservedSectorCount = rd16((const uint8_t*)&bs->reservedSectorCount);
    uint8_t tableCount = bs->tableCount;
    uint16_t rootEntryCount = rd16((const uint8_t*)&bs->rootEntryCount);
    uint16_t totalSectors16 = rd16((const uint8_t*)&bs->totalSectors16);
    uint16_t tableSize16 = rd16((const uint8_t*)&bs->tableSize16);
    uint32_t totalSectors32 = rd32((const uint8_t*)&bs->totalSectors32);

    if (bytesPerSector != 512 && bytesPerSector != 1024 &&
        bytesPerSector != 2048 && bytesPerSector != 4096)
        return FAT_ERR_INVALID_BPB;
    if (sectorsPerCluster == 0 ||
        (sectorsPerCluster & (sectorsPerCluster - 1)) != 0)
        return FAT_ERR_INVALID_BPB;

    // totalSectors16 wins when nonzero, else the 32-bit count (FAT32 style)
    uint64_t totSec = totalSectors16 != 0 ? totalSectors16 : totalSectors32;
    if (totSec == 0)
        return FAT_ERR_INVALID_BPB;

    // sectors per FAT table: tableSize16 wins when nonzero, else fatsz32
    // (only FAT32 volumes carry fatsz32; offset 36 is always within the
    // 512-byte boot sector checked above)
    const FatExtBS32* ext32 = (const FatExtBS32*)bs->extended_section;
    uint32_t fatsz32 = rd32((const uint8_t*)&ext32->fatsz32);
    uint64_t fatSize = tableSize16;
    if (fatSize == 0)
        fatSize = fatsz32;
    if (fatSize == 0)
        return FAT_ERR_INVALID_BPB;

    // region layout derived from the BPB (in sectors; 64-bit intermediates
    // because fatsz * tableCount can exceed 32 bits in hostile BPBs)
    uint64_t fatAreaSectors = fatSize * tableCount;
    uint64_t rootDirSectors =
        ((uint64_t)sizeof(DirectoryEntry) * rootEntryCount +
         bytesPerSector - 1) /
        bytesPerSector;
    uint64_t dataStart = reservedSectorCount + fatAreaSectors + rootDirSectors;
    // all derived regions must fit and leave at least one data cluster
    uint64_t clusters =
        dataStart < totSec ? (totSec - dataStart) / sectorsPerCluster : 0;
    if (clusters < 1)
        return FAT_ERR_INVALID_BPB;

    // classify by the data cluster count (spec thresholds)
    enum FAT_TYPE type;
    if (clusters < 4085)
        type = FT_FAT12;
    else if (clusters < 65525)
        type = FT_FAT16;
    else
        type = FT_FAT32;

    // per-type BPB requirements
    if (type != FT_FAT32 && tableSize16 == 0)
        return FAT_ERR_INVALID_BPB; // FAT12/16 must carry tableSize16
    if (type == FT_FAT16 && rootEntryCount == 0)
        return FAT_ERR_INVALID_BPB;
    if (type == FT_FAT32 && fatsz32 == 0)
        return FAT_ERR_INVALID_BPB;
    if (fatSize > 0xFFFF)
        return FAT_ERR_INVALID_BPB; // does not fit geo.fat_sectors

    uint32_t rootCluster = 0; // FAT12/16: fixed root region, no chain
    uint16_t fsInfoSector = 0;
    uint32_t activeFat = 0;
    uint16_t extFlags = 0;
    if (type == FT_FAT32) {
        extFlags = rd16((const uint8_t*)&ext32->extFlags);
        rootCluster = rd32((const uint8_t*)&ext32->rootClus);
        fsInfoSector = rd16((const uint8_t*)&ext32->fsInfo);
        // spec: rootClus must name a valid data cluster
        if (rootCluster < 2 || rootCluster >= 2 + clusters)
            return FAT_ERR_INVALID_BPB;
        // extFlags bit7 set: no mirroring, low 4 bits pick the active FAT
        if (extFlags & 0x0080)
            activeFat = extFlags & 0x000F;
        if (activeFat >= tableCount)
            return FAT_ERR_INVALID_BPB;
    }

    // the whole volume must actually fit the backend extent
    uint64_t imageSize = totSec * bytesPerSector;
    if (imageSize > backend_size)
        return FAT_ERR_INVALID_BPB;

    fat_ctx_t* ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
        return FAT_ERR_NOMEM;
    memset(ctx, 0, sizeof(*ctx)); // cache slots invalid, ext_flags 0
    ctx->cache_buf = malloc((size_t)FAT_CACHE_SLOTS * bytesPerSector);
    if (ctx->cache_buf == NULL) {
        free(ctx);
        return FAT_ERR_NOMEM;
    }
    for (int i = 0; i < FAT_CACHE_SLOTS; i++)
        ctx->cache[i].data = ctx->cache_buf + (size_t)i * bytesPerSector;
    ctx->io = io; // owned from here; fat_close flushes and closes it
    ctx->image_size = (size_t)backend_size;
    ctx->type = type;
    ctx->geo.bytes_per_sector = bytesPerSector;
    ctx->geo.sectors_per_cluster = sectorsPerCluster;
    ctx->geo.reserved_sectors = reservedSectorCount;
    ctx->geo.fat_count = tableCount;
    ctx->geo.fat_sectors = (uint16_t)fatSize;
    ctx->geo.root_entries = rootEntryCount;
    ctx->geo.total_sectors = (uint32_t)totSec;
    // FAT #0 for FAT12/16; the active table for FAT32
    ctx->geo.fat_start_sector =
        (uint32_t)(reservedSectorCount + (uint64_t)activeFat * fatSize);
    // FAT32 has no fixed root region: the root is the rootClus chain
    ctx->geo.root_dir_sector =
        type == FT_FAT32 ? 0 : (uint32_t)(reservedSectorCount + fatAreaSectors);
    ctx->geo.root_dir_sectors = type == FT_FAT32 ? 0 : (uint32_t)rootDirSectors;
    ctx->geo.root_cluster = rootCluster;
    ctx->geo.data_start_sector = (uint32_t)dataStart;
    ctx->geo.cluster_count = (uint32_t)clusters;
    ctx->fsinfo_sector = fsInfoSector;
    ctx->ext_flags = extFlags;

    *out = ctx;
    return FAT_OK;
}

// lifecycle ----------------------------------------------------------------

fat_result_t fat_open_io(fat_io_t* io, fat_ctx_t** out) {
    return fat_ctx_init(io, out);
}

fat_result_t fat_sync(fat_ctx_t* ctx) {
    if (ctx == NULL)
        return FAT_ERR_INVALID_ARG;
    return cache_flush_all(ctx);
}

void fat_close(fat_ctx_t* ctx) {
    if (ctx == NULL)
        return;
    (void)cache_flush_all(ctx); // best-effort; errors belong to fat_sync
    if (ctx->io != NULL && ctx->io->close != NULL)
        ctx->io->close(ctx->io); // exactly once, at close
    free(ctx->cache_buf);
    free(ctx);
}

// introspection ------------------------------------------------------------

enum FAT_TYPE fat_get_type(const fat_ctx_t* ctx) {
    if (ctx == NULL)
        return FT_UNKNOWN;
    return ctx->type;
}

const fat_geometry_t* fat_geometry(const fat_ctx_t* ctx) {
    if (ctx == NULL)
        return NULL;
    return &ctx->geo;
}

uint32_t fat_cluster_size(const fat_ctx_t* ctx) {
    if (ctx == NULL)
        return 0;
    return (uint32_t)ctx->geo.bytes_per_sector * ctx->geo.sectors_per_cluster;
}

fat_result_t fat_get_fat_entry(const fat_ctx_t* ctx, uint32_t cluster,
                               uint32_t* out) {
    if (ctx == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    // indices 0..cluster_count+1 are readable; 0/1 hold the media/reserved
    // entries
    if (cluster > ctx->geo.cluster_count + 1)
        return FAT_ERR_INVALID_ARG;
    uint32_t value;
    fat_result_t r = fat_raw_fat_entry(fat_ctx_mut(ctx), cluster, &value);
    if (r == FAT_ERR_IO)
        return r; // backend read failure
    if (r != FAT_OK)
        return FAT_ERR_INVALID_BPB; // FAT region does not cover the index
    *out = value;
    return FAT_OK;
}

// long file names (phase 7, §20) ---------------------------------------------

#define LFN_MAX_ENTRIES 20 // 13 UTF-16 units each: 255 chars + NUL
#define LFN_MAX_UNITS 255  // name units, terminator excluded

// short-name checksum over the 11 on-disk name bytes (fatgen103)
static uint8_t lfn_checksum11(const uint8_t name11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1u) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

// ASCII-range case-insensitive equality; bytes >= 0x80 compare exactly
// (consistent with fat_name_to_83's to-upper convention; §20.3)
static bool name_ci_eq(const char* a, const char* b) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    while (*pa != 0 && *pb != 0) {
        unsigned char ca =
            *pa >= 'a' && *pa <= 'z' ? (unsigned char)(*pa - 'a' + 'A') : *pa;
        unsigned char cb =
            *pb >= 'a' && *pb <= 'z' ? (unsigned char)(*pb - 'a' + 'A') : *pb;
        if (ca != cb)
            return false;
        pa++;
        pb++;
    }
    return *pa == 0 && *pb == 0;
}

// one LFN run in physical order: seq N (0x40 flag) first, down to seq 1
// directly before the 8.3 follower. `offs` keeps each entry's byte offset
// so deletion can invalidate the run slot by slot (§20.5).
typedef struct {
    uint8_t ents[LFN_MAX_ENTRIES][32];
    size_t offs[LFN_MAX_ENTRIES];
    uint32_t count;
    bool poisoned; // over LFN_MAX_ENTRIES entries: joins fail until reset
} LfnAcc;

static void lfn_acc_reset(LfnAcc* acc) {
    acc->count = 0;
    acc->poisoned = false;
}

static void lfn_acc_push(LfnAcc* acc, const uint8_t raw[32], size_t off) {
    if (acc->count >= LFN_MAX_ENTRIES) {
        acc->poisoned = true; // malformed directory (§20.2)
        return;
    }
    memcpy(acc->ents[acc->count], raw, sizeof(acc->ents[0]));
    acc->offs[acc->count] = off;
    acc->count++;
}

// UTF-16 name-unit offsets within one 32-byte LFN entry
static const int lfn_unit_off[13] = {1,  3,  5,  7,  9,  14, 16,
                                     18, 20, 22, 24, 28, 30};

// append cp as UTF-8; false when the NUL-terminated result would not fit
static bool utf8_put(char* out, size_t cap, size_t* o, uint32_t cp) {
    uint8_t b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (uint8_t)(0xC0 | (cp >> 6));
        b[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (uint8_t)(0xE0 | (cp >> 12));
        b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | (cp >> 18));
        b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*o + n + 1 > cap)
        return false; // no room for the NUL
    memcpy(out + *o, b, n);
    *o += n;
    return true;
}

// validate the accumulated run against the follower's 11-byte name and
// decode it (§20.2): descending sequence with the 0x40 flag on the physical
// first entry, checksum match on every entry, UTF-16LE -> UTF-8 with
// surrogate pairs. false -> the caller falls back to the 8.3 rendering.
static bool lfn_join(const LfnAcc* acc, const uint8_t name11[11],
                     char* out, size_t cap) {
    if (acc->poisoned || acc->count == 0)
        return false;
    uint32_t n = acc->count;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t seq = acc->ents[i][0];
        if ((seq & 0x3Fu) != (uint8_t)(n - i))
            return false; // sequence must descend N .. 1
        if ((seq & 0x40u) != (i == 0 ? 0x40u : 0x00u))
            return false; // only the physical first entry is flagged
    }
    uint8_t csum = lfn_checksum11(name11);
    for (uint32_t i = 0; i < n; i++)
        if (acc->ents[i][13] != csum)
            return false; // orphaned / mismatched run

    // flatten the units in logical order (seq 1 entry first); 0x0000 ends
    // the name, 0xFFFF is padding
    uint16_t us[LFN_MAX_ENTRIES * 13];
    size_t nu = 0;
    size_t o = 0;
    for (uint32_t i = n; i-- > 0;) {
        const uint8_t* ent = acc->ents[i];
        for (int j = 0; j < 13; j++) {
            uint16_t u = (uint16_t)(ent[lfn_unit_off[j]] |
                                    (ent[lfn_unit_off[j] + 1] << 8));
            if (u == 0x0000)
                goto units_done;
            if (u == 0xFFFF)
                continue; // padding
            us[nu++] = u;
        }
    }
units_done:
    for (size_t i = 0; i < nu;) {
        uint16_t u = us[i];
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= nu)
                return false; // lone high surrogate
            uint16_t v = us[i + 1];
            if (v < 0xDC00 || v > 0xDFFF)
                return false;
            cp = 0x10000u + (((uint32_t)u - 0xD800u) << 10) +
                 ((uint32_t)v - 0xDC00u);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false; // lone low surrogate
        } else {
            cp = u;
            i++;
        }
        if (!utf8_put(out, cap, &o, cp))
            return false; // UTF-8 form does not fit: 8.3 fallback (§20.2)
    }
    if (o == 0)
        return false; // empty name: not a usable LFN
    out[o] = '\0';
    return true;
}

// decode UTF-8 into UTF-16 units (surrogate pair for cp >= 0x10000);
// false: invalid UTF-8 or more than LFN_MAX_UNITS units
static bool utf8_to_utf16(const char* s, uint16_t units[LFN_MAX_UNITS],
                          size_t* count) {
    const unsigned char* p = (const unsigned char*)s;
    size_t n = 0;
    while (*p != 0) {
        uint32_t cp;
        int len;
        if (*p < 0x80) {
            cp = *p;
            len = 1;
        } else if ((*p & 0xE0u) == 0xC0u) {
            cp = *p & 0x1Fu;
            len = 2;
        } else if ((*p & 0xF0u) == 0xE0u) {
            cp = *p & 0x0Fu;
            len = 3;
        } else if ((*p & 0xF8u) == 0xF0u) {
            cp = *p & 0x07u;
            len = 4;
        } else {
            return false;
        }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0u) != 0x80u)
                return false; // truncated / malformed sequence
            cp = (cp << 6) | (p[i] & 0x3Fu);
        }
        // overlong forms, surrogates and out-of-range code points
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000))
            return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        p += len;
        if (cp >= 0x10000u) {
            if (n + 1 >= LFN_MAX_UNITS)
                return false;
            units[n++] = (uint16_t)(0xD800u + ((cp - 0x10000u) >> 10));
            units[n++] = (uint16_t)(0xDC00u + ((cp - 0x10000u) & 0x3FFu));
        } else {
            if (n >= LFN_MAX_UNITS)
                return false;
            units[n++] = (uint16_t)cp;
        }
    }
    *count = n;
    return n > 0;
}

// LFN write-side name analysis (§20.4): 1..255 UTF-8 bytes, 1..255 UTF-16
// units, no '/' or '\'. false -> FAT_ERR_NAME_TOO_LONG territory.
static bool lfn_name_units(const char* name, uint16_t units[LFN_MAX_UNITS],
                           size_t* count) {
    size_t bytes = strlen(name);
    if (bytes == 0 || bytes > 255)
        return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
        return false;
    return utf8_to_utf16(name, units, count);
}

// fill one 32-byte LFN entry: seq byte (0x40 on the logical last = physical
// first), the 8.3 checksum, and 13 UTF-16LE units from `units` (which
// includes the 0x0000 terminator; 0xFFFF padding past its end)
static void lfn_fill_entry(uint8_t raw[32], uint8_t seq, uint8_t csum,
                           const uint16_t* units, size_t n) {
    memset(raw, 0, 32);
    raw[0] = seq;
    raw[11] = ATTR_LONG_NAME;
    raw[12] = 0;              // type (always 0)
    raw[13] = csum;
    raw[26] = 0;              // firstClusterLow (always 0)
    raw[27] = 0;
    size_t base = (size_t)((seq & 0x3Fu) - 1u) * 13u;
    for (int i = 0; i < 13; i++) {
        uint16_t v = base + (size_t)i < n ? units[base + (size_t)i] : 0xFFFFu;
        raw[lfn_unit_off[i]] = (uint8_t)(v & 0xFFu);
        raw[lfn_unit_off[i] + 1] = (uint8_t)(v >> 8);
    }
}

// the ASCII characters an alias cannot carry (§20.4): controls and the
// invalid set -- each becomes a single '_'. Spaces and periods are not
// substitution material (they fold away); non-ASCII is per character, in
// the filter below
static bool lfn_alias_bad_char(unsigned char c) {
    if (c < 0x20)
        return true;
    return strchr("\"*+,;<=>?[\\]|:", c) != NULL;
}

// filter `src` (up to `len` bytes) into `dst` (capacity `cap`): spaces and
// periods fold away, each bad ASCII character becomes '_', each non-ASCII
// UTF-8 character becomes ONE '_' (lead ruling 2026-10-06; mtools 4.0.43
// measured: "a long name file" -> ALONGN~1, a 7-char Japanese name -> 7
// underscores), ASCII letters upcase. Returns the bytes written.
static size_t lfn_alias_filter(const char* src, size_t len, char* dst,
                               size_t cap) {
    size_t o = 0;
    size_t i = 0;
    while (i < len && o < cap) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '.') {
            i++;
            continue;
        }
        if (c >= 0x80) {
            dst[o++] = '_';
            i++;
            while (i < len && ((unsigned char)src[i] & 0xC0u) == 0x80u)
                i++; // skip the rest of the UTF-8 sequence
            continue;
        }
        if (lfn_alias_bad_char(c))
            c = '_';
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        dst[o++] = (char)c;
        i++;
    }
    return o;
}

// Windows-style alias BASE~N for a long name (§20.4): base prefix from the
// name start (lfn_alias_filter, prefix shrinks as N gains digits), ~N,
// space padding; extension from the last '.' (first 3 filtered chars, same
// filter)
static void lfn_make_alias(const char* name, unsigned long n,
                           uint8_t name11[11]) {
    int digits = 1;
    for (unsigned long v = n; v >= 10; v /= 10)
        digits++;
    size_t plen = (size_t)(8 - 1 - digits);

    const char* lastdot = strrchr(name, '.');
    size_t baselen = lastdot != NULL ? (size_t)(lastdot - name) : strlen(name);

    char base[8];
    size_t o = lfn_alias_filter(name, baselen, base, plen);
    memcpy(name11, base, o);
    name11[o++] = '~';
    char digs[8];
    int nd = 0;
    for (unsigned long v = n; v > 0; v /= 10)
        digs[nd++] = (char)('0' + v % 10);
    while (nd > 0)
        name11[o++] = (uint8_t)digs[--nd];
    while (o < 8)
        name11[o++] = ' ';

    char ext[3];
    size_t e = 0;
    if (lastdot != NULL)
        e = lfn_alias_filter(lastdot + 1, strlen(lastdot + 1), ext,
                             sizeof(ext));
    memcpy(&name11[8], ext, e);
    while (e < 3)
        name11[8 + e++] = ' ';
}

// directory iteration --------------------------------------------------------

// the 11-byte on-disk encodings of the dot entries
static const uint8_t fat_dot11[11] = {'.', ' ', ' ', ' ', ' ', ' ',
                                      ' ', ' ', ' ', ' ', ' '};
static const uint8_t fat_dotdot11[11] = {'.', '.', ' ', ' ', ' ', ' ',
                                         ' ', ' ', ' ', ' ', ' '};

// stepwise iterator over one directory; the single scan implementation
// behind fat_dir_next. No image pointers are held: each 32-byte entry is
// read into `raw` through the cache on demand.
struct fat_dir {
    fat_ctx_t* ctx;
    bool fixed;         // FAT12/16 fixed root region (not a cluster chain)
    bool done;          // sticky exhaustion (further next calls stay at END)
    uint32_t root_next; // fixed mode: next slot index in the root region
    uint32_t cluster;   // chain mode: current cluster
    uint32_t slot;      // chain mode: next entry slot within the cluster
    uint32_t visited;   // chain mode: clusters entered so far (loop cap)
    uint32_t fat_next;  // chain mode: FAT value of the current cluster
    uint64_t cluster_off; // chain mode: byte offset of the current cluster
    LfnAcc lfn;         // LFN entries accumulated before the next 8.3 slot
    fat_dirent_t entry; // cursor-owned storage handed to the caller
    uint8_t raw[32];    // ditto; valid until the next fat_dir_next/close
};

// DOS invariant check: every subdirectory starts with a "." self-entry
// (first 32 bytes: '.' + 10 spaces, ATTR_DIRECTORY); data or garbage here
// is not a directory. Reads the 12 leading bytes through the cache.
static fat_result_t dir_check_dot(fat_ctx_t* ctx, uint32_t cluster) {
    uint64_t off;
    if (!cluster_offset(ctx, cluster, &off))
        return FAT_ERR_INVALID_ARG;
    uint8_t head[12];
    fat_result_t r = fat_io_read(ctx, off, head, sizeof(head));
    if (r != FAT_OK)
        return r;
    if (memcmp(head, fat_dot11, 11) != 0 || (head[11] & ATTR_DIRECTORY) == 0)
        return FAT_ERR_INVALID_ARG;
    return FAT_OK;
}

// enter one cluster of a directory chain: in-image and not marked bad
static fat_result_t dir_cluster_enter(fat_ctx_t* ctx, uint32_t cluster,
                                      uint32_t* fat_next, uint64_t* data_off) {
    uint64_t off;
    if (!cluster_offset(ctx, cluster, &off))
        return FAT_ERR_BAD_CLUSTER;
    uint32_t fat;
    fat_result_t r = fat_raw_fat_entry(ctx, cluster, &fat);
    if (r == FAT_ERR_IO)
        return r;
    if (r != FAT_OK || fat_is_bad(ctx, fat))
        return FAT_ERR_BAD_CLUSTER;
    *fat_next = fat;
    *data_off = off;
    return FAT_OK;
}

// the FAT12/16 root directory is a fixed region, not a chain; it must lie
// inside the image (shared by the cursor and the write-side scan)
static fat_result_t root_region_check(const fat_ctx_t* ctx) {
    uint64_t offset = (uint64_t)ctx->geo.root_dir_sector *
                      ctx->geo.bytes_per_sector;
    uint64_t bytes = (uint64_t)ctx->geo.root_dir_sectors *
                     ctx->geo.bytes_per_sector;
    if (offset > (uint64_t)ctx->image_size ||
        bytes > (uint64_t)ctx->image_size - offset)
        return FAT_ERR_INVALID_BPB;
    return FAT_OK;
}

// enter the directory a walk targets (chain mode: the FAT32 root sentinel
// or its real cluster number, or any subdirectory cluster): bounds, the
// subdirectory dot invariant (the root is exempt), then the first chain
// step. *cluster receives the entered cluster.
static fat_result_t dir_walk_start(fat_ctx_t* ctx, uint32_t dir_cluster,
                                   uint32_t* cluster, uint32_t* fat_next,
                                   uint64_t* data_off) {
    if (dir_cluster == FAT_CLUSTER_ROOT)
        dir_cluster = ctx->geo.root_cluster;
    if (dir_cluster < 2 || dir_cluster >= 2 + ctx->geo.cluster_count)
        return FAT_ERR_INVALID_ARG;
    if (!(ctx->type == FT_FAT32 && dir_cluster == ctx->geo.root_cluster)) {
        fat_result_t r = dir_check_dot(ctx, dir_cluster);
        if (r != FAT_OK)
            return r;
    }
    *cluster = dir_cluster;
    return dir_cluster_enter(ctx, dir_cluster, fat_next, data_off);
}

fat_result_t fat_dir_open(fat_ctx_t* ctx, uint32_t dir_cluster,
                          fat_dir_t** out) {
    if (ctx == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;

    fat_dir_t* d = calloc(1, sizeof(*d));
    if (d == NULL)
        return FAT_ERR_NOMEM;
    d->ctx = ctx;

    if (dir_cluster == FAT_CLUSTER_ROOT && ctx->type != FT_FAT32) {
        // the FAT12/16 root directory is a fixed region, not a chain
        fat_result_t r = root_region_check(ctx);
        if (r != FAT_OK) {
            free(d);
            return r;
        }
        d->fixed = true;
        *out = d;
        return FAT_OK;
    }

    // chain mode: the FAT32 root (sentinel or its real cluster number) or
    // any subdirectory cluster
    fat_result_t r = dir_walk_start(ctx, dir_cluster, &d->cluster,
                                    &d->fat_next, &d->cluster_off);
    if (r != FAT_OK) {
        free(d);
        return r;
    }
    d->visited = 1;
    *out = d;
    return FAT_OK;
}

// feed one just-read 32-byte slot (at byte offset `off`) to the LFN
// accumulator / entry builder; true when d->entry holds a yieldable entry
// (the 8.3 slot -- its name replaced by the joined LFN when the run
// validates). raw32 callbacks always receive the 8.3 bytes (§20.2).
static bool dir_slot_feed(fat_dir_t* d, uint64_t off) {
    if (d->raw[0] == 0xE5) {
        lfn_acc_reset(&d->lfn); // a deleted slot breaks any run
        return false;
    }
    if (d->raw[11] == ATTR_LONG_NAME) {
        lfn_acc_push(&d->lfn, d->raw, (size_t)off);
        return false;
    }
    dirent_from_raw(d->ctx, d->raw, &d->entry);
    char lfn[FAT_NAME_MAX];
    if (lfn_join(&d->lfn, d->raw, lfn, sizeof(lfn)))
        memcpy(d->entry.name, lfn, strlen(lfn) + 1);
    lfn_acc_reset(&d->lfn);
    return true;
}

fat_result_t fat_dir_next(fat_dir_t* d, const fat_dirent_t** out,
                          const uint8_t** raw32) {
    if (d == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    if (d->done)
        return FAT_ERR_END_OF_DIR; // sticky exhaustion

    if (d->fixed) {
        fat_ctx_t* ctx = d->ctx;
        uint64_t base = (uint64_t)ctx->geo.root_dir_sector *
                        ctx->geo.bytes_per_sector;
        while (d->root_next < ctx->geo.root_entries) {
            uint64_t off =
                base + (uint64_t)d->root_next++ * sizeof(DirectoryEntry);
            fat_result_t r = fat_io_read(ctx, off, d->raw, sizeof(d->raw));
            if (r != FAT_OK)
                return r;
            if (d->raw[0] == 0x00) { // no more entries
                lfn_acc_reset(&d->lfn);
                break;
            }
            if (dir_slot_feed(d, off)) {
                *out = &d->entry;
                if (raw32 != NULL)
                    *raw32 = d->raw;
                return FAT_OK;
            }
        }
        d->done = true;
        return FAT_ERR_END_OF_DIR;
    }

    uint32_t entriesPerCluster =
        fat_cluster_size(d->ctx) / (uint32_t)sizeof(DirectoryEntry);
    for (;;) {
        bool end_of_dir = false;
        while (d->slot < entriesPerCluster) {
            uint64_t off =
                d->cluster_off + (uint64_t)d->slot++ * sizeof(DirectoryEntry);
            fat_result_t r = fat_io_read(d->ctx, off, d->raw, sizeof(d->raw));
            if (r != FAT_OK)
                return r;
            if (d->raw[0] == 0x00) { // no more entries
                lfn_acc_reset(&d->lfn);
                end_of_dir = true;
                break;
            }
            if (dir_slot_feed(d, off)) {
                *out = &d->entry;
                if (raw32 != NULL)
                    *raw32 = d->raw;
                return FAT_OK;
            }
        }
        if (end_of_dir)
            break;

        // this cluster is exhausted: follow the chain with the shared
        // guards (free/reserved, bounds, visited cap; a bad-marked link
        // is caught by dir_cluster_enter on the next step)
        uint32_t next;
        chain_link_t cl = chain_link(d->ctx, d->fat_next, &next);
        if (cl == CHAIN_BROKEN ||
            (cl == CHAIN_NEXT && d->visited > d->ctx->geo.cluster_count))
            return FAT_ERR_BAD_CLUSTER; // broken link, or a loop
        if (cl == CHAIN_END)
            break; // natural end of the directory
        fat_result_t r =
            dir_cluster_enter(d->ctx, next, &d->fat_next, &d->cluster_off);
        if (r != FAT_OK)
            return r;
        d->cluster = next;
        d->visited++;
        d->slot = 0;
    }
    d->done = true;
    return FAT_ERR_END_OF_DIR;
}

void fat_dir_close(fat_dir_t* d) {
    free(d); // NULL-safe
}

// 8.3 name conversion --------------------------------------------------------

fat_result_t fat_name_from_83(const uint8_t name11[11], uint8_t attributes,
                              char* out, size_t out_len) {
    if (name11 == NULL || out == NULL || out_len == 0)
        return FAT_ERR_INVALID_ARG;

    // a stored FIRST byte of 0x05 is the Japanese-name escape for a true
    // lead byte of 0xE5 (stored differently so it is not mistaken for a
    // deleted entry); 0x05 anywhere else is a literal byte
    uint8_t render[11];
    memcpy(render, name11, 11);
    if (render[0] == 0x05)
        render[0] = 0xE5;

    // base: 8 bytes, trailing spaces trimmed
    size_t baseLen = 0;
    while (baseLen < 8 && render[baseLen] != 0x20)
        baseLen++;

    // directories and volume labels get no extension dot; a file with an
    // all-blank extension gets none either
    size_t extLen = 0;
    if ((attributes & (ATTR_DIRECTORY | ATTR_VOLUME_ID)) == 0) {
        extLen = 3;
        while (extLen > 0 && render[8 + extLen - 1] == 0x20)
            extLen--;
    }
    size_t len = baseLen + (extLen > 0 ? 1 + extLen : 0);

    if (len + 1 > out_len)
        return FAT_ERR_BUFFER_TOO_SMALL;

    memcpy(out, render, baseLen);
    size_t o = baseLen;
    if (extLen > 0) {
        out[o++] = '.';
        memcpy(&out[o], &render[8], extLen);
        o += extLen;
    }
    out[o] = '\0';
    return FAT_OK;
}

fat_result_t fat_name_to_83(const char* name, uint8_t name11[11]) {
    if (name == NULL || name11 == NULL)
        return FAT_ERR_INVALID_ARG;
    if (name[0] == '\0')
        return FAT_ERR_INVALID_ARG;
    // fat_lookup handles "." and ".." itself; they are not disk names
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return FAT_ERR_INVALID_ARG;

    // split into the 8.3 base and extension at the first '.'
    const char* dot = strchr(name, '.');
    size_t baseLen = dot != NULL ? (size_t)(dot - name) : strlen(name);
    const char* ext = dot != NULL ? dot + 1 : NULL;
    size_t extLen = ext != NULL ? strlen(ext) : 0;

    // names that do not fit in 8.3 are not representable
    if (baseLen > 8 || extLen > 3)
        return FAT_ERR_NAME_TOO_LONG;

    memcpy(name11, name, baseLen);
    memset(&name11[baseLen], 0x20, 8 - baseLen);
    memset(&name11[8], 0x20, 3);
    if (extLen > 0)
        memcpy(&name11[8], ext, extLen);

    // to upper
    for (int i = 0; i < 11; i++) {
        if ('a' <= name11[i] && name11[i] <= 'z')
            name11[i] = (uint8_t)(name11[i] - 'a' + 'A');
    }
    return FAT_OK;
}

// timestamps -----------------------------------------------------------------

fat_result_t fat_dos_date_to_tm(uint16_t dos_date, uint16_t dos_time,
                                uint8_t dos_tenth, struct tm* out) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;

    // bit fields: date = (year-1980)<<9 | month<<5 | day,
    //             time = hour<<11 | min<<5 | sec/2
    uint32_t month = (dos_date >> 5) & 0x0F;
    uint32_t day = dos_date & 0x1F;
    uint32_t hour = dos_time >> 11;
    uint32_t minute = (dos_time >> 5) & 0x3F;
    uint32_t sec2 = dos_time & 0x1F; // 2-second units

    // reject fields a valid DOS stamp cannot encode (day-of-month upper
    // bound stays per-field: 1..31; the calendar is not checked)
    if (month < 1 || month > 12 || day < 1)
        return FAT_ERR_INVALID_ARG;
    if (hour > 23 || minute > 59 || sec2 > 29)
        return FAT_ERR_INVALID_ARG;
    if (dos_tenth > 199)
        return FAT_ERR_INVALID_ARG; // 0.01s units: max 1.99s

    // zero-init, then fill: struct tm carries more fields than DOS encodes
    memset(out, 0, sizeof(*out));
    out->tm_year = (int)(1980 + (dos_date >> 9)) - 1900;
    out->tm_mon = (int)month - 1;
    out->tm_mday = (int)day;
    out->tm_hour = (int)hour;
    out->tm_min = (int)minute;
    out->tm_sec = (int)(sec2 * 2);
    out->tm_isdst = 0;
    return FAT_OK;
}

// FSInfo (FAT32 only) ----------------------------------------------------------

// read the FSInfo sector and validate its signature triple ("RRaA" @0,
// "rrAa" @484, 0xAA550000 @508). FAT_OK + true: the valid sector is in
// `sec`; FAT_OK + false: absent or signature-invalid (the "unknown" state);
// otherwise the backend read error.
static fat_result_t fsinfo_read_valid(fat_ctx_t* ctx, uint8_t sec[512],
                                      bool* valid) {
    *valid = false;
    if (ctx->fsinfo_sector == 0)
        return FAT_OK; // 0 = no FSInfo sector
    uint64_t offset = (uint64_t)ctx->fsinfo_sector * ctx->geo.bytes_per_sector;
    if (offset + 512 > (uint64_t)ctx->image_size)
        return FAT_OK; // sector absent from the image
    fat_result_t r = fat_io_read(ctx, offset, sec, 512);
    if (r != FAT_OK)
        return r;
    if (rd32(sec) != 0x41615252 ||       // "RRaA"
        rd32(sec + 484) != 0x61417272 || // "rrAa"
        rd32(sec + 508) != 0xAA550000)
        return FAT_OK; // invalid signature
    *valid = true;
    return FAT_OK;
}

fat_result_t fat_fsinfo(const fat_ctx_t* ctx, fat_fsinfo_t* out) {
    if (ctx == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    out->free_cluster_count = 0xFFFFFFFF; // unknown/stale
    out->next_free_cluster = 0xFFFFFFFF;
    if (ctx->type != FT_FAT32)
        return FAT_ERR_UNSUPPORTED;

    uint8_t sec[512];
    bool valid;
    fat_result_t r = fsinfo_read_valid(fat_ctx_mut(ctx), sec, &valid);
    if (r != FAT_OK)
        return r;
    if (valid) {
        out->free_cluster_count = rd32(sec + 488);
        out->next_free_cluster = rd32(sec + 492);
    }
    return FAT_OK;
}

// path lookup ------------------------------------------------------------------

// search one directory for the name (dir_scan query); false when absent.
// Defined after dir_scan -- fat_lookup and the write APIs share it.
static bool lookup_in_dir(fat_ctx_t* ctx, uint32_t dir_cluster,
                          const char* name, fat_dirent_t* out);

// synthesize a dirent for a "." / ".." that is not backed by an on-disk
// entry (the root is its own parent)
static fat_dirent_t dot_dirent(const char* name, uint32_t cluster) {
    fat_dirent_t d;
    memset(&d, 0, sizeof(d));
    d.name[0] = '.';
    if (name[1] != '\0')
        d.name[1] = '.';
    d.attributes = ATTR_DIRECTORY;
    d.first_cluster = cluster;
    return d;
}

fat_result_t fat_lookup(fat_ctx_t* ctx, uint32_t start_cluster,
                        const char* path, fat_dirent_t* out) {
    if (ctx == NULL || path == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;

    // copy the path so we can tokenize it (components are short: 8.3)
    char tmp[512];
    if (strlen(path) >= sizeof(tmp))
        return FAT_ERR_NAME_TOO_LONG;
    memcpy(tmp, path, strlen(path) + 1);

    uint32_t cur = start_cluster;

    // tokenize once per component; `next` peeks at the following one so only
    // the last component may be a file
    char* saveptr = NULL;
    char* token = strtok_r(tmp, "/", &saveptr);
    while (token != NULL) {
        char* next = strtok_r(NULL, "/", &saveptr);
        const bool last = next == NULL;

        if (strcmp(token, ".") == 0) {
            // "." stays in the current directory; at the root there is no
            // on-disk dot entry, so synthesize one
            if (last) {
                if (cur == FAT_CLUSTER_ROOT) {
                    *out = dot_dirent(".", FAT_CLUSTER_ROOT);
                    return FAT_OK;
                }
                fat_dirent_t self;
                if (!lookup_in_dir(ctx, cur, ".", &self))
                    return FAT_ERR_PATH_NOT_FOUND;
                *out = self;
                return FAT_OK;
            }
            token = next;
            continue;
        }
        if (strcmp(token, "..") == 0) {
            // the root is its own parent: ".." at the root stays there
            // (transitive chains like "dir1/../.." keep resolving)
            if (cur == FAT_CLUSTER_ROOT) {
                if (last) {
                    *out = dot_dirent("..", FAT_CLUSTER_ROOT);
                    return FAT_OK;
                }
                token = next;
                continue;
            }
            // resolve via the directory's own ".." entry
            fat_dirent_t dotdot;
            if (!lookup_in_dir(ctx, cur, "..", &dotdot))
                return FAT_ERR_PATH_NOT_FOUND;
            uint32_t parent = dotdot.first_cluster;
            // some tools record the root as 0; FAT32 tools may also record
            // the root's real cluster number -- both normalize to the root
            // sentinel
            if (parent == 0 || parent == ctx->geo.root_cluster)
                parent = FAT_CLUSTER_ROOT;
            if (last) {
                *out = dot_dirent("..", parent);
                return FAT_OK;
            }
            cur = parent;
            token = next;
            continue;
        }

        // §20.3: components match by rendered name (LFN preferred, 8.3
        // fallback, ASCII case-insensitive) -- long components resolve too
        fat_dirent_t hit;
        if (!lookup_in_dir(ctx, cur, token, &hit)) {
            return last ? FAT_ERR_NOT_FOUND : FAT_ERR_PATH_NOT_FOUND;
        }
        if (last) {
            *out = hit;
            return FAT_OK;
        }
        if ((hit.attributes & ATTR_DIRECTORY) == 0)
            return FAT_ERR_PATH_NOT_FOUND; // intermediate must be a dir
        cur = hit.first_cluster;
        token = next;
    }

    // only empty components ("", "/", "//"): nothing to resolve
    return FAT_ERR_INVALID_ARG;
}

// file read --------------------------------------------------------------------

// Brent's cycle detection (O(1) space, one compare per step). A healthy
// chain never revisits a cluster; a self-pointing FAT entry would
// otherwise satisfy file_size with garbage. The tortoise lags behind the
// advancing head by a power-of-two stride.
typedef struct {
    uint32_t tortoise; // compare anchor
    uint32_t power;    // current stride length
    uint32_t lam;      // steps taken within the current stride
} Brent;

static void brent_init(Brent* b, uint32_t start) {
    b->tortoise = start;
    b->power = 1;
    b->lam = 1;
}

// true when `cluster` was already visited (a cycle); advances the anchors
// otherwise. Call only for links that passed every other guard, so the
// state advances exactly once per real step.
static bool brent_revisited(Brent* b, uint32_t cluster) {
    if (cluster == b->tortoise)
        return true;
    if (b->power == b->lam) {
        b->tortoise = cluster;
        b->power <<= 1;
        b->lam = 0;
    } else {
        b->lam++;
    }
    return false;
}

fat_result_t fat_read_file(fat_ctx_t* ctx, const fat_dirent_t* file,
                           uint8_t** out, size_t* out_size) {
    if (ctx == NULL || file == NULL || out == NULL || out_size == NULL)
        return FAT_ERR_INVALID_ARG;

    *out = NULL;
    *out_size = 0;

    // ATTR_LONG_NAME (0x0F) includes the volume-id bit, so the mask below
    // rejects LFN entries as well
    if (file->attributes & (ATTR_DIRECTORY | ATTR_VOLUME_ID))
        return FAT_ERR_INVALID_ARG;

    if (file->file_size == 0)
        return FAT_OK; // empty file

    uint32_t cluster = file->first_cluster;
    if (cluster < 2 || cluster >= 2 + ctx->geo.cluster_count)
        return FAT_ERR_BAD_CLUSTER;

    uint32_t cluster_size = fat_cluster_size(ctx);

    uint8_t* buf = malloc(file->file_size);
    if (buf == NULL)
        return FAT_ERR_NOMEM;

    // chain-read exactly file_size bytes; the visited cap and Brent's
    // cycle detection below guard against loops in a corrupt FAT
    Brent brent;
    brent_init(&brent, cluster);
    uint32_t remaining = file->file_size;
    uint32_t copied = 0;
    for (uint32_t visited = 0;; visited++) {
        uint64_t coff;
        bool in_image = cluster_offset(ctx, cluster, &coff);
        uint32_t fat = 0;
        fat_result_t fr = in_image ? fat_raw_fat_entry(ctx, cluster, &fat)
                                   : FAT_ERR_INVALID_BPB;
        if (fr == FAT_ERR_IO) {
            free(buf);
            return fr; // backend read failure
        }
        if (!in_image || fr != FAT_OK || fat_is_bad(ctx, fat))
            break; // broken

        uint32_t n = remaining < cluster_size ? remaining : cluster_size;
        fat_result_t r = fat_io_read(ctx, coff, buf + copied, n);
        if (r != FAT_OK) {
            free(buf);
            return r;
        }
        copied += n;
        remaining -= n;
        if (remaining == 0) {
            *out = buf;
            *out_size = file->file_size;
            return FAT_OK;
        }

        // follow the FAT: any unhealthy link ends the walk (EOC short of
        // file_size, free/reserved mid-chain, bad value, out of bounds, a
        // chain longer than the image, or a revisit)
        uint32_t next;
        if (chain_link(ctx, fat, &next) != CHAIN_NEXT ||
            visited >= ctx->geo.cluster_count ||
            brent_revisited(&brent, next))
            break; // broken
        cluster = next;
    }

    // no partial buffer on error
    free(buf);
    return FAT_ERR_BAD_CLUSTER;
}

// streaming file access ----------------------------------------------------------

// read cursor over one file's cluster chain; the dirent is copied at open,
// the context is borrowed (fat_close(ctx) before fat_file_close is a
// caller error). Write cursors (fat_file_open_write) additionally bind to
// the dirent's on-disk slot so truncate/write can patch first-cluster and
// file-size in place.
struct fat_file {
    fat_ctx_t* ctx;
    fat_dirent_t dirent; // copy: callers may pass stack objects
    uint64_t pos;        // absolute byte position, always <= size
    uint64_t size;
    uint32_t cluster;    // cluster holding `pos` (valid while pos < size)
    uint32_t index;      // chain index of `cluster`
    bool writable;       // false for read cursors (fat_file_open)
    uint64_t slot_offset; // writable only: byte offset of the dirent slot
};

fat_result_t fat_file_open(fat_ctx_t* ctx, const fat_dirent_t* file,
                           fat_file_t** out) {
    if (ctx == NULL || file == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;

    // same rules as fat_read_file: only regular files are readable
    // (ATTR_LONG_NAME (0x0F) includes the volume-id bit, so the mask below
    // rejects LFN entries as well)
    if (file->attributes & (ATTR_DIRECTORY | ATTR_VOLUME_ID))
        return FAT_ERR_INVALID_ARG;

    fat_file_t* f = calloc(1, sizeof(*f));
    if (f == NULL)
        return FAT_ERR_NOMEM;
    f->ctx = ctx;
    f->dirent = *file;
    f->size = file->file_size;
    f->cluster = file->first_cluster; // chain errors surface at seek/read
    *out = f;
    return FAT_OK;
}

uint64_t fat_file_tell(const fat_file_t* f) {
    return f == NULL ? 0 : f->pos;
}

uint64_t fat_file_size(const fat_file_t* f) {
    return f == NULL ? 0 : f->size;
}

// follow `steps` FAT links from `first`, with fat_read_file's guards:
// bad values, broken links (EOC short of the target, free/reserved, out
// of bounds) and Brent cycle detection
static fat_result_t chain_step(fat_ctx_t* ctx, uint32_t first,
                               uint32_t steps, uint32_t* out) {
    uint32_t cluster = first;
    Brent brent;
    brent_init(&brent, first);
    for (uint32_t i = 0; i < steps; i++) {
        uint32_t fat;
        fat_result_t r = fat_raw_fat_entry(ctx, cluster, &fat);
        if (r == FAT_ERR_IO)
            return r;
        uint32_t next;
        if (r != FAT_OK || fat_is_bad(ctx, fat) ||
            chain_link(ctx, fat, &next) != CHAIN_NEXT ||
            brent_revisited(&brent, next))
            return FAT_ERR_BAD_CLUSTER;
        cluster = next;
    }
    *out = cluster;
    return FAT_OK;
}

fat_result_t fat_file_seek(fat_file_t* f, uint64_t offset) {
    if (f == NULL)
        return FAT_ERR_INVALID_ARG;
    if (offset > f->size)
        return FAT_ERR_INVALID_ARG; // past EOF (== size is EOF and legal)

    if (offset < f->size) {
        // O(chain length): re-walk from the first cluster
        uint32_t index = (uint32_t)(offset / fat_cluster_size(f->ctx));
        uint32_t cluster;
        fat_result_t r =
            chain_step(f->ctx, f->dirent.first_cluster, index, &cluster);
        if (r != FAT_OK)
            return r;
        f->cluster = cluster;
        f->index = index;
    }
    // offset == size needs no cluster: reads there deliver EOF
    f->pos = offset;
    return FAT_OK;
}

fat_result_t fat_file_read(fat_file_t* f, uint8_t* buf, size_t len,
                           size_t* read) {
    if (f == NULL || buf == NULL || read == NULL)
        return FAT_ERR_INVALID_ARG;
    *read = 0;

    uint64_t remaining = f->size - f->pos;
    if (len == 0 || remaining == 0)
        return FAT_OK; // EOF: 0 bytes delivered

    // a short read only happens at EOF: deliver the full len otherwise
    uint64_t want = len < (uint64_t)remaining ? (uint64_t)len : remaining;
    uint32_t cluster_size = fat_cluster_size(f->ctx);

    uint8_t* o = buf;
    uint64_t left = want;
    uint32_t cluster = f->cluster;
    uint32_t index = f->index;
    uint32_t offset_in = (uint32_t)(f->pos % cluster_size);

    // Brent anchors for this call's walk (same detection as fat_read_file)
    Brent brent;
    brent_init(&brent, cluster);
    uint32_t steps = 0;

    while (left > 0) {
        uint64_t coff;
        if (!cluster_offset(f->ctx, cluster, &coff))
            return FAT_ERR_BAD_CLUSTER;
        uint32_t avail = cluster_size - offset_in;
        uint64_t n = left < avail ? left : avail;
        fat_result_t r = fat_io_read(f->ctx, coff + offset_in, o, (size_t)n);
        if (r != FAT_OK)
            return r;
        o += n;
        left -= n;
        offset_in += (uint32_t)n;
        if (left == 0)
            break;

        // need the next cluster: follow the FAT with fat_read_file's
        // guards (bad/broken link, chain longer than the image, or a
        // revisit all end the read)
        uint32_t fat;
        r = fat_raw_fat_entry(f->ctx, cluster, &fat);
        if (r == FAT_ERR_IO)
            return r;
        uint32_t next;
        if (r != FAT_OK || fat_is_bad(f->ctx, fat) ||
            chain_link(f->ctx, fat, &next) != CHAIN_NEXT ||
            steps + 1 > f->ctx->geo.cluster_count ||
            brent_revisited(&brent, next))
            return FAT_ERR_BAD_CLUSTER; // chain ends before the file does
        cluster = next;
        index++;
        steps++;
        offset_in = 0;
    }

    f->pos += want;
    f->cluster = cluster;
    f->index = index;
    *read = (size_t)want;
    return FAT_OK;
}

void fat_file_close(fat_file_t* f) {
    free(f); // NULL-safe
}

// write support -------------------------------------------------------------------

// the exact EOC constant per image type (what fat_alloc_cluster marks)
static uint32_t fat_eoc_const(const fat_ctx_t* ctx) {
    return fat_type_info[ctx->type].eoc_const;
}

// FSInfo free-count / next-free write-back (FAT32 only). A sector whose
// signatures do not validate is left untouched, and an unknown free count
// stays unknown (symmetric with fat_fsinfo's read-side degradation).
static fat_result_t fsinfo_adjust(fat_ctx_t* ctx, int64_t free_delta,
                                  const uint32_t* next_free) {
    if (ctx->type != FT_FAT32)
        return FAT_OK;
    uint8_t sec[512];
    bool valid;
    fat_result_t r = fsinfo_read_valid(ctx, sec, &valid);
    if (r != FAT_OK || !valid)
        return r; // absent/invalid: keep the unknown state
    uint64_t offset = (uint64_t)ctx->fsinfo_sector * ctx->geo.bytes_per_sector;

    uint32_t free_count = rd32(sec + 488);
    if (free_count != 0xFFFFFFFF) {
        uint8_t fc[4];
        wr32(fc, (uint32_t)((int64_t)free_count + free_delta));
        r = fat_io_write(ctx, offset + 488, fc, sizeof(fc));
        if (r != FAT_OK)
            return r;
    }
    if (next_free != NULL) {
        uint8_t nf[4];
        wr32(nf, *next_free);
        r = fat_io_write(ctx, offset + 492, nf, sizeof(nf));
        if (r != FAT_OK)
            return r;
    }
    return FAT_OK;
}

fat_result_t fat_set_fat_entry(fat_ctx_t* ctx, uint32_t cluster,
                               uint32_t value) {
    if (ctx == NULL)
        return FAT_ERR_INVALID_ARG;
    if (cluster < 2 || cluster > ctx->geo.cluster_count + 1)
        return FAT_ERR_INVALID_ARG;

    // byte span of the entry within one table (fat_entry_span, the same
    // math the decoder uses) and the per-type value ceiling (eoc_const)
    uint64_t ent;
    size_t need;
    if (!fat_entry_span(ctx->type, cluster, &ent, &need))
        return FAT_ERR_INVALID_ARG;
    if (value > fat_type_info[ctx->type].eoc_const)
        return FAT_ERR_INVALID_ARG;

    // all mirrors by default; only the active table when BPB_ExtFlags
    // disables mirroring (FAT32). Table k sits at reserved + k*fat_sectors
    // (geo.fat_start_sector already points at the active table, so the
    // base is derived from reserved_sectors here to cover every k)
    uint32_t first_table = 0;
    uint32_t table_count = ctx->geo.fat_count;
    if (ctx->type == FT_FAT32 && (ctx->ext_flags & 0x0080)) {
        first_table = ctx->ext_flags & 0x000F;
        table_count = 1;
    }

    size_t fat_bytes = (size_t)ctx->geo.fat_sectors *
                       ctx->geo.bytes_per_sector;
    if (need > fat_bytes)
        return FAT_ERR_INVALID_BPB; // FAT region does not cover the index

    for (uint32_t t = first_table; t < first_table + table_count; t++) {
        uint64_t base = ((uint64_t)ctx->geo.reserved_sectors +
                         (uint64_t)t * ctx->geo.fat_sectors) *
                        ctx->geo.bytes_per_sector;
        fat_result_t r;
        switch (ctx->type) {
        case FT_FAT16: {
            uint8_t raw[2];
            wr16(raw, (uint16_t)value);
            r = fat_io_write(ctx, base + ent, raw, sizeof(raw));
            break;
        }
        case FT_FAT32: {
            uint8_t raw[4];
            // the upper 4 bits are reserved flags: keep each table's own
            // on-disk value (read-modify-write)
            r = fat_io_read(ctx, base + ent, raw, sizeof(raw));
            if (r != FAT_OK)
                return r;
            wr32(raw, (rd32(raw) & 0xF0000000u) | value);
            r = fat_io_write(ctx, base + ent, raw, sizeof(raw));
            break;
        }
        case FT_FAT12: {
            // read-modify-write: neighbouring entries share a byte (the
            // same packing fat_raw_fat_entry decodes; odd entries start at
            // floor(cluster*3/2), one byte past (cluster/2)*3)
            uint8_t pair[2];
            r = fat_io_read(ctx, base + ent, pair, sizeof(pair));
            if (r != FAT_OK)
                return r;
            if (cluster % 2 == 0) {
                pair[0] = (uint8_t)(value & 0xFF);
                pair[1] =
                    (uint8_t)((pair[1] & 0xF0) | ((value >> 8) & 0x0F));
            } else {
                pair[0] =
                    (uint8_t)((pair[0] & 0x0F) | ((value & 0x0F) << 4));
                pair[1] = (uint8_t)((value >> 4) & 0xFF);
            }
            r = fat_io_write(ctx, base + ent, pair, sizeof(pair));
            break;
        }
        default:
            return FAT_ERR_INVALID_ARG;
        }
        if (r != FAT_OK)
            return r;
    }
    return FAT_OK;
}

fat_result_t fat_alloc_cluster(fat_ctx_t* ctx, uint32_t* out) {
    if (ctx == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = 0;

    // valid entries are 2..cluster_count+1; the FAT32 FSInfo hint seeds the
    // scan when it names a valid cluster (stale values fall back to 2)
    uint32_t total = ctx->geo.cluster_count;
    uint32_t start = 2;
    if (ctx->type == FT_FAT32) {
        fat_fsinfo_t fi;
        if (fat_fsinfo(ctx, &fi) == FAT_OK && fi.next_free_cluster >= 2 &&
            fi.next_free_cluster <= total + 1)
            start = fi.next_free_cluster;
    }

    uint32_t found = 0;
    for (uint32_t i = 0; i < total && found == 0; i++) {
        uint32_t c = start + i;
        if (c > total + 1)
            c -= total; // wrap to the lowest entries
        uint32_t v;
        fat_result_t rr = fat_raw_fat_entry(ctx, c, &v);
        if (rr == FAT_ERR_IO)
            return rr;
        if (rr == FAT_OK && v == 0)
            found = c; // an index the FAT region does not cover is not free
    }
    if (found == 0)
        return FAT_ERR_DISK_FULL; // the scan is read-only: image unchanged

    fat_result_t r = fat_set_fat_entry(ctx, found, fat_eoc_const(ctx));
    if (r != FAT_OK)
        return r;

    // keep the FSInfo in step: one fewer free cluster, and the hint moves
    // to the scan's next candidate
    uint32_t next = found + 1 > total + 1 ? 2 : found + 1;
    r = fsinfo_adjust(ctx, -1, &next);
    if (r != FAT_OK)
        return r;
    *out = found;
    return FAT_OK;
}

fat_result_t fat_free_chain(fat_ctx_t* ctx, uint32_t head) {
    if (ctx == NULL)
        return FAT_ERR_INVALID_ARG;
    if (head < 2 || head > ctx->geo.cluster_count + 1)
        return FAT_ERR_INVALID_ARG;

    // Walk with the read-side guards, zeroing every entry as it is left
    // (all required copies, via fat_set_fat_entry). Zero-as-you-go also
    // breaks cycles: a revisited cluster reads as free mid-chain.
    uint32_t freed = 0;
    uint32_t c = head;
    for (uint32_t visited = 0;; visited++) {
        uint32_t fat;
        fat_result_t rr = fat_raw_fat_entry(ctx, c, &fat);
        if (rr == FAT_ERR_IO)
            return rr;
        if (rr != FAT_OK)
            return FAT_ERR_BAD_CLUSTER;
        fat_result_t r = fat_set_fat_entry(ctx, c, 0);
        if (r != FAT_OK)
            return r;
        freed++;
        uint32_t next;
        chain_link_t cl = chain_link(ctx, fat, &next);
        if (cl == CHAIN_END)
            break; // natural end of the chain
        if (cl == CHAIN_BROKEN || fat_is_bad(ctx, fat) ||
            visited > ctx->geo.cluster_count)
            return FAT_ERR_BAD_CLUSTER; // bad/free/reserved mid-chain, or
                                        // longer than the image: a loop
        c = next;
    }

    // the freed head is the best next-free candidate
    return fsinfo_adjust(ctx, (int64_t)freed, &head);
}

// one-pass LFN-aware directory scan (§20.3/§20.4): the first live name
// collision (rendered-name matching), the first reusable slot run, the
// 0x00 region, (chain mode) the tail cluster an extension links onto, and
// -- on demand -- every live 8.3 name for alias collision numbering.
// Walks with the fat_dir cursor's rules and guards.
typedef struct {
    // query
    const char* qname;   // rendered name to match (NULL: no name query)
    uint32_t need;       // consecutive slots a new entry requires (>= 1)
    bool collect_names;  // record live 8.3 name11s (alias collisions)
    // match results (valid when exists)
    bool exists;
    size_t match_offset;     // byte offset of the matched 8.3 slot
    uint8_t match_name11[11];
    char match_lfn[FAT_NAME_MAX]; // the matched entry's LFN rendering
    bool match_has_lfn;           // (a checksum-valid run preceded the slot)
    size_t lfn_offsets[LFN_MAX_ENTRIES]; // checksum-valid run at the match
    uint32_t lfn_count;                  // (§20.5: what unlink invalidates)
    // allocation results
    bool have_del_run;   // first run of consecutive 0xE5 slots that fits
    size_t del_run_offset;
    bool have_free;      // first 0x00 slot (the directory ends there)
    size_t free_offset;
    uint32_t free_slots; // 0x00 slots left in that cluster / region
    bool fixed;          // FAT12/16 fixed root region: cannot extend
    uint32_t tail;       // chain mode: last cluster of the chain
    // alias collision set (malloc'd; free with dir_scan_cleanup)
    uint8_t (*names)[11];
    size_t name_count;
    size_t name_cap;
    // scan-internal deleted-run state
    bool run_open;
    size_t run_start;
    uint32_t run_len;
} DirScan;

static void dir_scan_cleanup(DirScan* s) {
    free(s->names);
    s->names = NULL;
    s->name_count = 0;
    s->name_cap = 0;
}

// record one live 8.3 name for alias collision numbering
static fat_result_t dir_scan_name_add(DirScan* s, const uint8_t raw[32]) {
    if (s->name_count == s->name_cap) {
        size_t cap = s->name_cap == 0 ? 16 : s->name_cap * 2;
        uint8_t (*grown)[11] = realloc(s->names, cap * sizeof(*s->names));
        if (grown == NULL)
            return FAT_ERR_NOMEM;
        s->names = grown;
        s->name_cap = cap;
    }
    memcpy(s->names[s->name_count], raw, 11);
    s->name_count++;
    return FAT_OK;
}

// feed one just-read 32-byte slot at byte offset `off` into the scan;
// `slots_left` counts this slot through the end of its cluster / region.
// *stop: stop scanning (0x00 region reached, or the query matched).
static fat_result_t dir_scan_feed(DirScan* s, LfnAcc* acc, const uint8_t raw[32],
                                  size_t off, uint32_t slots_left, bool* stop) {
    *stop = false;
    if (raw[0] == 0x00) { // never used: the directory ends here
        lfn_acc_reset(acc);
        s->have_free = true;
        s->free_offset = off;
        s->free_slots = slots_left;
        *stop = true;
        return FAT_OK;
    }
    if (raw[0] == 0xE5) {
        lfn_acc_reset(acc); // a deleted slot breaks any run
        if (!s->run_open) {
            s->run_open = true;
            s->run_start = off;
            s->run_len = 0;
        }
        s->run_len++;
        if (!s->have_del_run && s->run_len >= s->need) {
            s->have_del_run = true; // first fitting run wins (§20.4)
            s->del_run_offset = s->run_start;
        }
        return FAT_OK;
    }
    // a live slot closes any deleted run
    s->run_open = false;
    if (raw[11] == ATTR_LONG_NAME) {
        lfn_acc_push(acc, raw, off);
        return FAT_OK; // long file name fragment, not an 8.3 name
    }
    if (s->collect_names) {
        fat_result_t r = dir_scan_name_add(s, raw);
        if (r != FAT_OK)
            return r;
    }
    if (s->qname != NULL && !s->exists && (raw[11] & ATTR_VOLUME_ID) == 0) {
        // labels are metadata, never a name match
        char lfn[FAT_NAME_MAX];
        char rendered[FAT_NAME_MAX];
        bool has_lfn = lfn_join(acc, raw, lfn, sizeof(lfn));
        fat_name_from_83(raw, raw[11], rendered, sizeof(rendered));
        if (name_ci_eq(s->qname, has_lfn ? lfn : rendered) ||
            (has_lfn && name_ci_eq(s->qname, rendered))) {
            s->exists = true;
            s->match_offset = off;
            memcpy(s->match_name11, raw, 11);
            s->match_has_lfn = has_lfn; // path lookup re-renders from the slot
            if (has_lfn)
                memcpy(s->match_lfn, lfn, strlen(lfn) + 1);
            // the checksum-valid part of the preceding run dies with this
            // slot (§20.5); mismatching orphans stay
            uint8_t csum = lfn_checksum11(raw);
            for (uint32_t i = 0; i < acc->count; i++)
                if (acc->ents[i][13] == csum)
                    s->lfn_offsets[s->lfn_count++] = acc->offs[i];
            *stop = true;
            return FAT_OK;
        }
    }
    lfn_acc_reset(acc);
    return FAT_OK;
}

static fat_result_t dir_scan(fat_ctx_t* ctx, uint32_t dir_cluster,
                             DirScan* out) {
    LfnAcc acc;
    lfn_acc_reset(&acc);

    if (dir_cluster == FAT_CLUSTER_ROOT && ctx->type != FT_FAT32) {
        // the FAT12/16 root directory is a fixed region, not a chain
        out->fixed = true;
        fat_result_t r = root_region_check(ctx);
        if (r != FAT_OK)
            return r;
        uint64_t offset = (uint64_t)ctx->geo.root_dir_sector *
                          ctx->geo.bytes_per_sector;
        size_t slots = (size_t)(ctx->geo.root_dir_sectors *
                                ctx->geo.bytes_per_sector /
                                sizeof(DirectoryEntry));
        if (slots > ctx->geo.root_entries)
            slots = ctx->geo.root_entries;
        for (size_t s = 0; s < slots; s++) {
            uint64_t off = offset + (uint64_t)s * sizeof(DirectoryEntry);
            uint8_t raw[32];
            r = fat_io_read(ctx, off, raw, sizeof(raw));
            if (r != FAT_OK)
                return r;
            bool stop;
            r = dir_scan_feed(out, &acc, raw, (size_t)off,
                              (uint32_t)(slots - s), &stop);
            if (r != FAT_OK)
                return r;
            if (stop)
                return FAT_OK;
        }
        return FAT_OK;
    }

    // chain mode: the FAT32 root (sentinel or its real cluster number) or
    // any subdirectory -- the same entry sequence as fat_dir_open
    uint32_t cluster;
    uint32_t fat_next;
    uint64_t data_off;
    fat_result_t r = dir_walk_start(ctx, dir_cluster, &cluster, &fat_next,
                                    &data_off);
    if (r != FAT_OK)
        return r;
    uint32_t entries_per_cluster =
        fat_cluster_size(ctx) / (uint32_t)sizeof(DirectoryEntry);
    uint32_t visited = 1;
    for (;;) {
        for (uint32_t slot = 0; slot < entries_per_cluster; slot++) {
            uint64_t off = data_off + (uint64_t)slot * sizeof(DirectoryEntry);
            uint8_t raw[32];
            r = fat_io_read(ctx, off, raw, sizeof(raw));
            if (r != FAT_OK)
                return r;
            bool stop;
            r = dir_scan_feed(out, &acc, raw, (size_t)off,
                              entries_per_cluster - slot, &stop);
            if (r != FAT_OK)
                return r;
            if (stop) {
                // the 0x00 region (or a match): this cluster is the link
                // point for any chain extension
                out->tail = cluster;
                return FAT_OK;
            }
        }

        // this cluster is exhausted: follow the chain with the cursor's
        // guards (free/reserved, bounds, visited cap; a bad-marked link
        // is caught by dir_cluster_enter on the next step)
        uint32_t next;
        chain_link_t cl = chain_link(ctx, fat_next, &next);
        if (cl == CHAIN_BROKEN ||
            (cl == CHAIN_NEXT && visited > ctx->geo.cluster_count))
            return FAT_ERR_BAD_CLUSTER; // broken link, or a loop
        if (cl == CHAIN_END) {
            out->tail = cluster; // a healthy end: an extension links here
            break;
        }
        r = dir_cluster_enter(ctx, next, &fat_next, &data_off);
        if (r != FAT_OK)
            return r;
        cluster = next;
        visited++;
    }
    return FAT_OK;
}

// search one directory for the name; false when absent. A dir_scan name
// query does the matching (LFN rendering preferred, 8.3 alias fallback,
// ASCII case-insensitive, labels excluded) and records where the 8.3 slot
// sits; the dirent is then re-read from that slot, its name replaced by the
// LFN rendering when one validated.
static bool lookup_in_dir(fat_ctx_t* ctx, uint32_t dir_cluster,
                          const char* name, fat_dirent_t* out) {
    DirScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.qname = name;
    scan.need = 1;
    if (dir_scan(ctx, dir_cluster, &scan) != FAT_OK)
        return false;
    if (!scan.exists)
        return false;

    uint8_t raw[32];
    if (fat_io_read(ctx, scan.match_offset, raw, sizeof(raw)) != FAT_OK)
        return false;
    dirent_from_raw(ctx, raw, out);
    if (scan.match_has_lfn)
        memcpy(out->name, scan.match_lfn, strlen(scan.match_lfn) + 1);
    return true;
}

// zero `len` bytes at `offset`, in bounded chunks (an extension can be
// larger than any sane stack buffer)
static fat_result_t write_zeros(fat_ctx_t* ctx, uint64_t offset, size_t len) {
    static const uint8_t zero[4096];
    while (len > 0) {
        size_t n = len > sizeof(zero) ? sizeof(zero) : len;
        fat_result_t r = fat_io_write(ctx, offset, zero, n);
        if (r != FAT_OK)
            return r;
        offset += n;
        len -= n;
    }
    return FAT_OK;
}

// extend a directory chain (chain mode only) with enough zeroed clusters
// for `want_slots` entries, linked after `tail`; *first_off receives the
// first new cluster's byte offset. Zeroed before linking so the 0x00
// terminator semantics never break mid-crash. On failure the chain is
// restored to its original FAT value and the grown part freed.
static fat_result_t dir_extend_chain(fat_ctx_t* ctx, uint32_t tail,
                                     uint32_t want_slots,
                                     uint64_t* first_off) {
    uint32_t epc = fat_cluster_size(ctx) / (uint32_t)sizeof(DirectoryEntry);
    uint32_t want_clusters = ((uint64_t)want_slots + epc - 1) / epc;

    uint32_t fat_orig;
    fat_result_t r = fat_raw_fat_entry(ctx, tail, &fat_orig);
    if (r != FAT_OK)
        return r;

    uint64_t head_off = 0;
    uint32_t grown_head = 0;
    uint32_t prev = tail;
    for (uint32_t i = 0; i < want_clusters; i++) {
        uint32_t nc;
        r = fat_alloc_cluster(ctx, &nc);
        if (r != FAT_OK)
            break;
        uint64_t cl_off;
        if (!cluster_offset(ctx, nc, &cl_off)) {
            fat_free_chain(ctx, nc);
            r = FAT_ERR_INVALID_BPB;
            break;
        }
        r = write_zeros(ctx, cl_off, fat_cluster_size(ctx));
        if (r != FAT_OK) {
            fat_free_chain(ctx, nc);
            break;
        }
        r = fat_set_fat_entry(ctx, prev, nc);
        if (r != FAT_OK) {
            fat_free_chain(ctx, nc);
            break; // nc leaks (EOC-marked); the chain stays consistent
        }
        if (i == 0) {
            head_off = cl_off;
            grown_head = nc;
        }
        prev = nc;
    }
    if (r != FAT_OK) {
        if (grown_head != 0) {
            fat_set_fat_entry(ctx, tail, fat_orig);
            fat_free_chain(ctx, grown_head);
        }
        return r;
    }
    *first_off = head_off;
    return FAT_OK;
}

// name routing (§20.4), shared by the two creators: separators are never
// storable, 8.3-representable names stay on the pure 8.3 path (one slot),
// longer names must satisfy the LFN limits. name11 is filled on the 8.3
// path; on the LFN path `units`/`nunits` carry the encoded name and
// *n_lfn the LFN slot count (0 on the 8.3 path; NULL = not wanted).
static fat_result_t name_route(const char* name, uint8_t name11[11],
                               uint16_t units[LFN_MAX_UNITS], size_t* nunits,
                               uint32_t* n_lfn) {
    if (n_lfn != NULL)
        *n_lfn = 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
        return FAT_ERR_NAME_TOO_LONG; // fat_name_to_83 alone would accept
                                      // e.g. "bad/name.txt" (base fits 8)
    fat_result_t r = fat_name_to_83(name, name11);
    if (r != FAT_ERR_NAME_TOO_LONG)
        return r;
    if (!lfn_name_units(name, units, nunits))
        return FAT_ERR_NAME_TOO_LONG; // not LFN-encodable either
    if (n_lfn != NULL)
        *n_lfn = (uint32_t)((*nunits + 1 + 12) / 13); // units + terminator
    return FAT_OK;
}

fat_result_t fat_add_dirent(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const fat_dirent_t* tmpl) {
    if (ctx == NULL || name == NULL)
        return FAT_ERR_INVALID_ARG;

    uint8_t name11[11];
    uint16_t units[LFN_MAX_UNITS];
    size_t nunits = 0;
    uint32_t n_lfn = 0;
    fat_result_t r = name_route(name, name11, units, &nunits, &n_lfn);
    if (r != FAT_OK)
        return r;

    // EXISTS is decided before anything is allocated (a failed add must
    // not move the free-cluster count)
    DirScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.qname = name;
    scan.need = n_lfn + 1;
    scan.collect_names = n_lfn > 0; // alias collision numbering needs them
    r = dir_scan(ctx, dir_cluster, &scan);
    if (r != FAT_OK) {
        dir_scan_cleanup(&scan);
        return r;
    }
    if (scan.exists) {
        dir_scan_cleanup(&scan);
        return FAT_ERR_EXISTS;
    }

    if (n_lfn > 0) {
        // generate the Windows-style alias: the smallest ~N whose 8.3 name
        // is free among the directory's live entries (§20.4)
        bool have_alias = false;
        for (unsigned long n = 1; n <= 999999ul; n++) {
            uint8_t cand[11];
            lfn_make_alias(name, n, cand);
            bool collision = false;
            for (size_t i = 0; i < scan.name_count && !collision; i++)
                if (memcmp(scan.names[i], cand, 11) == 0)
                    collision = true;
            if (!collision) {
                memcpy(name11, cand, 11);
                have_alias = true;
                break;
            }
        }
        dir_scan_cleanup(&scan);
        if (!have_alias)
            return FAT_ERR_DIR_FULL; // no free alias number left
    } else {
        dir_scan_cleanup(&scan);
    }

    // the on-disk name comes from the analysis above; every other field
    // from tmpl (NULL = ATTR_ARCHIVE and zero timestamps)
    fat_dirent_t def;
    if (tmpl == NULL) {
        memset(&def, 0, sizeof(def));
        def.attributes = 0x20; // ATTR_ARCHIVE
        tmpl = &def;
    }
    DirectoryEntry e;
    memset(&e, 0, sizeof(e));
    memcpy(e.name, name11, 11);
    e.attributes = tmpl->attributes;
    e.creationTimeTenthOfSecond = tmpl->creation_time_tenth;
    e.creationTime = tmpl->creation_time;
    e.creationDate = tmpl->creation_date;
    e.lastAccessDate = tmpl->last_access_date;
    // first cluster bytes 20/21/26/27 (the dirent_from_raw mirror)
    raw_set_first_cluster(ctx->type, (uint8_t*)&e, tmpl->first_cluster);
    e.lastWriteTime = tmpl->last_write_time;
    e.lastWriteDate = tmpl->last_write_date;
    e.fileSize = tmpl->file_size;

    // slot selection (§20.4): the first 0xE5 run that fits, else the 0x00
    // region, else (chain mode) zero-filled extension clusters
    uint64_t offset;
    if (scan.have_del_run) {
        offset = scan.del_run_offset; // reuse from the run head
    } else if (scan.have_free && scan.free_slots >= scan.need) {
        offset = scan.free_offset;
    } else if (scan.fixed) {
        return FAT_ERR_DIR_FULL; // the fixed root region cannot extend
    } else {
        // extend the chain: entries may start in the 0x00 remainder of
        // the tail cluster and spill into the new zeroed clusters
        uint32_t deficit =
            scan.have_free ? scan.need - scan.free_slots : scan.need;
        uint64_t ext_off;
        r = dir_extend_chain(ctx, scan.tail, deficit, &ext_off);
        if (r != FAT_OK)
            return r;
        offset = scan.have_free ? scan.free_offset : ext_off;
    }

    // crash-consistent order (§20.4): the LFN run lands before the 8.3
    // entry, so an interruption orphans at worst -- never a live name
    // whose run is missing
    uint8_t raw32[sizeof(DirectoryEntry)];
    uint8_t csum = lfn_checksum11(name11);
    uint16_t with_term[LFN_MAX_UNITS + 1];
    if (n_lfn > 0) {
        memcpy(with_term, units, nunits * sizeof(uint16_t));
        with_term[nunits] = 0; // 0x0000 terminator
        for (uint32_t i = 0; i < n_lfn; i++) {
            uint8_t seq = (uint8_t)(n_lfn - i); // physical: highest first
            lfn_fill_entry(raw32, (uint8_t)(seq | (i == 0 ? 0x40 : 0x00)),
                           csum, with_term, nunits + 1);
            r = fat_io_write(ctx, offset + (uint64_t)i * sizeof(raw32),
                             raw32, sizeof(raw32));
            if (r != FAT_OK)
                return r;
        }
    }
    memcpy(raw32, &e, sizeof(raw32));
    return fat_io_write(ctx, offset + (uint64_t)n_lfn * sizeof(raw32),
                        raw32, sizeof(raw32));
}

// allocate the data chain for `size` bytes and copy them in, cluster by
// cluster (the final partial cluster keeps the tail bytes of whatever was
// there: file_size bounds readers). *head receives the first cluster, 0
// when nothing was allocated. Any failure frees what it allocated.
static fat_result_t write_data_chain(fat_ctx_t* ctx, const uint8_t* data,
                                     size_t size, uint32_t* head) {
    *head = 0;
    uint32_t cluster_size = fat_cluster_size(ctx);

    uint64_t left = size;
    uint32_t prev = 0;
    while (left > 0) {
        uint32_t nc;
        fat_result_t r = fat_alloc_cluster(ctx, &nc);
        if (r != FAT_OK) {
            if (*head != 0)
                fat_free_chain(ctx, *head);
            return r;
        }
        if (prev == 0) {
            *head = nc; // keeps the EOC fat_alloc_cluster wrote
        } else {
            r = fat_set_fat_entry(ctx, prev, nc);
            if (r != FAT_OK) {
                fat_free_chain(ctx, *head);
                return r;
            }
        }
        size_t n = left < cluster_size ? (size_t)left : (size_t)cluster_size;
        uint64_t off;
        if (!cluster_offset(ctx, nc, &off)) {
            fat_free_chain(ctx, *head);
            return FAT_ERR_INVALID_BPB;
        }
        r = fat_io_write(ctx, off, data + (size - left), n);
        if (r != FAT_OK) {
            fat_free_chain(ctx, *head);
            return r;
        }
        left -= n;
        prev = nc;
    }
    return FAT_OK;
}

fat_result_t fat_write_file(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const uint8_t* data,
                            size_t size, const fat_dirent_t* tmpl) {
    if (ctx == NULL || name == NULL)
        return FAT_ERR_INVALID_ARG;
    if (data == NULL && size > 0)
        return FAT_ERR_INVALID_ARG;

    // reject up front what fat_add_dirent cannot store (§20.4), so nothing
    // is allocated before the name checks out
    uint8_t name11[11];
    uint16_t units[LFN_MAX_UNITS];
    size_t nunits = 0;
    fat_result_t r = name_route(name, name11, units, &nunits, NULL);
    if (r != FAT_OK)
        return r;

    // an existing name (LFN rendering or 8.3 alias) is refused before
    // anything is allocated (a failed write must not move the
    // free-cluster count)
    fat_dirent_t hit;
    if (lookup_in_dir(ctx, dir_cluster, name, &hit))
        return FAT_ERR_EXISTS;

    // crash-consistent order: data chain first, dirent last
    uint32_t head = 0;
    if (size > 0) {
        r = write_data_chain(ctx, data, size, &head);
        if (r != FAT_OK)
            return r;
    }

    // the dirent template: tmpl supplies attributes and timestamps; the
    // first cluster and size are ours (NULL tmpl = the D3 defaults)
    fat_dirent_t t;
    if (tmpl == NULL) {
        memset(&t, 0, sizeof(t));
        t.attributes = 0x20; // ATTR_ARCHIVE
    } else {
        t = *tmpl;
    }
    t.first_cluster = head;
    t.file_size = (uint32_t)size;
    r = fat_add_dirent(ctx, dir_cluster, name, &t);
    if (r != FAT_OK) {
        if (head != 0)
            fat_free_chain(ctx, head); // roll the allocation back
        return r;
    }
    return FAT_OK;
}

// deletion + write cursors (phase 6) ------------------------------------------

// write `first_cluster` / `file_size` back into the cursor's directory
// slot (read-modify-write: every other on-disk byte, timestamps included,
// stays untouched)
static fat_result_t dirent_slot_patch(fat_file_t* f, uint32_t first_cluster,
                                      uint32_t file_size) {
    uint8_t raw[32];
    fat_result_t r = fat_io_read(f->ctx, f->slot_offset, raw, sizeof(raw));
    if (r != FAT_OK)
        return r;
    raw_set_first_cluster(f->ctx->type, raw, first_cluster);
    wr32(raw + 28, file_size);
    return fat_io_write(f->ctx, f->slot_offset, raw, sizeof(raw));
}

// recompute the cursor's cluster bookkeeping for `pos` (chain_step walk,
// same guards as every other walker)
static fat_result_t file_cursor_resync(fat_file_t* f) {
    if (f->pos >= f->size) {
        // EOF: reads deliver 0 without touching the cluster, and a seek
        // re-walks anyway
        f->cluster = f->dirent.first_cluster;
        f->index = 0;
        return FAT_OK;
    }
    uint32_t idx =
        (uint32_t)(f->pos / fat_cluster_size(f->ctx));
    uint32_t c;
    fat_result_t r = chain_step(f->ctx, f->dirent.first_cluster, idx, &c);
    if (r != FAT_OK)
        return r;
    f->cluster = c;
    f->index = idx;
    return FAT_OK;
}

// consistency order shared by unlink/rmdir: kill the entry first -- the
// checksum-valid LFN run, then the 8.3 slot (§20.5) -- so an interruption
// may leak the chain (fsck recovers) but never leaves a live dirent
// pointing at freed clusters
static fat_result_t dir_slot_delete(fat_ctx_t* ctx, const DirScan* scan,
                                    uint32_t cluster) {
    static const uint8_t deleted = 0xE5;
    for (uint32_t i = 0; i < scan->lfn_count; i++) {
        fat_result_t r = fat_io_write(ctx, scan->lfn_offsets[i], &deleted, 1);
        if (r != FAT_OK)
            return r;
    }
    fat_result_t r = fat_io_write(ctx, scan->match_offset, &deleted, 1);
    if (r != FAT_OK)
        return r;
    if (cluster < 2)
        return FAT_OK; // empty file/directory: nothing to free
    return fat_free_chain(ctx, cluster);
}

// name-taking deletion/cursor APIs share one guard set: only disk names
// are valid (§20.3) -- "." / ".." never resolve, empty never resolves,
// and names beyond the LFN bounds cannot be on disk
static fat_result_t name_query_check(const char* name) {
    if (name == NULL)
        return FAT_ERR_INVALID_ARG;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return FAT_ERR_INVALID_ARG;
    if (strlen(name) > 255)
        return FAT_ERR_NAME_TOO_LONG;
    return FAT_OK;
}

fat_result_t fat_unlink(fat_ctx_t* ctx, uint32_t dir_cluster,
                        const char* name) {
    if (ctx == NULL)
        return FAT_ERR_INVALID_ARG;
    fat_result_t r = name_query_check(name);
    if (r != FAT_OK)
        return r;

    DirScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.qname = name;
    scan.need = 1;
    r = dir_scan(ctx, dir_cluster, &scan);
    if (r != FAT_OK)
        return r;
    if (!scan.exists)
        return FAT_ERR_NOT_FOUND;

    uint8_t raw[32];
    r = fat_io_read(ctx, scan.match_offset, raw, sizeof(raw));
    if (r != FAT_OK)
        return r;
    if (raw[11] & ATTR_DIRECTORY)
        return FAT_ERR_INVALID_ARG; // directories go through fat_rmdir
    if (raw[11] & ATTR_READ_ONLY)
        return FAT_ERR_INVALID_ARG; // the DOS access-denied equivalent

    return dir_slot_delete(ctx, &scan, raw_first_cluster(ctx->type, raw));
}

// rmdir emptiness check over the cursor: "." and ".." never count (the
// iteration itself skips deleted, never-used and LFN slots)
static fat_result_t dir_has_live_entry(fat_ctx_t* ctx, uint32_t cluster,
                                       bool* non_dot) {
    *non_dot = false;
    fat_dir_t* d = NULL;
    fat_result_t r = fat_dir_open(ctx, cluster, &d);
    if (r != FAT_OK)
        return r;
    const fat_dirent_t* entry;
    const uint8_t* raw32;
    while ((r = fat_dir_next(d, &entry, &raw32)) == FAT_OK) {
        if (memcmp(raw32, fat_dot11, 11) != 0 &&
            memcmp(raw32, fat_dotdot11, 11) != 0) {
            *non_dot = true;
            break;
        }
    }
    fat_dir_close(d);
    return r == FAT_ERR_END_OF_DIR ? FAT_OK : r;
}

fat_result_t fat_rmdir(fat_ctx_t* ctx, uint32_t dir_cluster,
                       const char* name) {
    if (ctx == NULL)
        return FAT_ERR_INVALID_ARG;
    fat_result_t r = name_query_check(name);
    if (r != FAT_OK)
        return r; // "." / ".." are not removable names

    DirScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.qname = name;
    scan.need = 1;
    r = dir_scan(ctx, dir_cluster, &scan);
    if (r != FAT_OK)
        return r;
    if (!scan.exists)
        return FAT_ERR_NOT_FOUND;

    uint8_t raw[32];
    r = fat_io_read(ctx, scan.match_offset, raw, sizeof(raw));
    if (r != FAT_OK)
        return r;
    if ((raw[11] & ATTR_DIRECTORY) == 0)
        return FAT_ERR_INVALID_ARG; // a regular file goes through fat_unlink
    if (raw[11] & ATTR_READ_ONLY)
        return FAT_ERR_INVALID_ARG;
    uint32_t cluster = raw_first_cluster(ctx->type, raw);
    // the root itself cannot be removed: the FAT12/16 fixed region carries
    // no dirent (cluster 0 in a corrupt one) and the FAT32 root chain is
    // geo.root_cluster
    if (cluster < 2 || cluster == ctx->geo.root_cluster)
        return FAT_ERR_INVALID_ARG;

    bool non_dot;
    r = dir_has_live_entry(ctx, cluster, &non_dot);
    if (r != FAT_OK)
        return r;
    if (non_dot)
        return FAT_ERR_DIR_NOT_EMPTY;

    return dir_slot_delete(ctx, &scan, cluster);
}

fat_result_t fat_file_open_write(fat_ctx_t* ctx, uint32_t dir_cluster,
                                 const char* name, fat_file_t** out) {
    if (ctx == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    fat_result_t r = name_query_check(name);
    if (r != FAT_OK)
        return r;

    DirScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.qname = name;
    scan.need = 1;
    r = dir_scan(ctx, dir_cluster, &scan);
    if (r != FAT_OK)
        return r;
    if (!scan.exists)
        return FAT_ERR_NOT_FOUND; // creation stays with fat_write_file

    uint8_t raw[32];
    r = fat_io_read(ctx, scan.match_offset, raw, sizeof(raw));
    if (r != FAT_OK)
        return r;
    // only regular files: ATTR_LONG_NAME (0x0F) carries the volume-id bit,
    // so the mask rejects LFN entries and labels alike
    if (raw[11] & (ATTR_DIRECTORY | ATTR_VOLUME_ID))
        return FAT_ERR_INVALID_ARG;
    if (raw[11] & ATTR_READ_ONLY)
        return FAT_ERR_INVALID_ARG;

    fat_file_t* f = calloc(1, sizeof(*f));
    if (f == NULL)
        return FAT_ERR_NOMEM;
    f->ctx = ctx;
    dirent_from_raw(ctx, raw, &f->dirent);
    f->size = f->dirent.file_size;
    f->cluster = f->dirent.first_cluster; // chain errors surface at read
    f->writable = true;
    f->slot_offset = scan.match_offset;
    *out = f;
    return FAT_OK;
}

fat_result_t fat_file_truncate(fat_file_t* f, uint64_t size) {
    if (f == NULL || !f->writable)
        return FAT_ERR_INVALID_ARG;
    if (size > 0xFFFFFFFFu)
        return FAT_ERR_INVALID_ARG; // the dirent's 32-bit fileSize caps it
    if (size == f->size)
        return FAT_OK; // nothing to do (pos <= size already)

    fat_ctx_t* ctx = f->ctx;
    uint32_t cluster_size = fat_cluster_size(ctx);
    uint32_t head = f->dirent.first_cluster;
    fat_result_t r;

    if (size == 0) {
        // dirent first (size 0, first_cluster 0), then the chain: the
        // reverse order could leave size > 0 over a freed chain
        r = dirent_slot_patch(f, 0, 0);
        if (r != FAT_OK)
            return r;
        f->dirent.first_cluster = 0;
        f->dirent.file_size = 0;
        f->size = 0;
        f->pos = 0;
        f->cluster = 0;
        f->index = 0;
        if (head >= 2) {
            r = fat_free_chain(ctx, head);
            if (r != FAT_OK)
                return r;
        }
        return FAT_OK;
    }

    if (size < f->size) {
        // shrink: the dirent size moves first so no intermediate state
        // claims bytes of a chain that is about to be freed
        r = dirent_slot_patch(f, head, (uint32_t)size);
        if (r != FAT_OK)
            return r;
        f->dirent.file_size = (uint32_t)size;
        f->size = size;
        if (f->pos > size)
            f->pos = size;

        // keep ceil(size / cluster_size) clusters, cut and free the rest
        uint32_t keep = (uint32_t)((size + cluster_size - 1) / cluster_size);
        uint32_t last;
        r = chain_step(ctx, head, keep - 1, &last);
        if (r != FAT_OK)
            return r;
        uint32_t fat;
        r = fat_raw_fat_entry(ctx, last, &fat);
        if (r != FAT_OK)
            return r;
        if (!fat_is_eoc(ctx, fat)) {
            if (fat < 2 || fat > ctx->geo.cluster_count + 1)
                return FAT_ERR_BAD_CLUSTER; // free/bad/reserved mid-chain
            r = fat_set_fat_entry(ctx, last, fat_eoc_const(ctx));
            if (r != FAT_OK)
                return r;
            r = fat_free_chain(ctx, fat);
            if (r != FAT_OK)
                return r;
        }
        return file_cursor_resync(f);
    }

    // grow -----------------------------------------------------------------
    uint64_t old = f->size;
    // stale bytes of the last partial cluster become visible: zero them
    // (up to the new end when the growth stays inside the cluster, up to
    // the cluster end when new clusters follow)
    if (old % cluster_size != 0) {
        uint64_t old_full =
            (old / cluster_size + 1) * cluster_size;
        uint64_t zend = size < old_full ? size : old_full;
        uint32_t c;
        r = chain_step(ctx, head, (uint32_t)(old / cluster_size), &c);
        if (r != FAT_OK)
            return r;
        uint64_t coff;
        if (!cluster_offset(ctx, c, &coff))
            return FAT_ERR_BAD_CLUSTER;
        r = write_zeros(ctx, coff + old, (size_t)(zend - old));
        if (r != FAT_OK)
            return r;
    }

    uint32_t need = (uint32_t)((size + cluster_size - 1) / cluster_size);
    uint32_t have = (uint32_t)((old + cluster_size - 1) / cluster_size);
    uint32_t extra = need - have;
    if (old == 0 && head >= 2) {
        // a size-0 entry that still names a chain: the chain holds no file
        // bytes -- release it and grow as the empty-file case
        r = fat_free_chain(ctx, head);
        if (r != FAT_OK)
            return r;
        r = dirent_slot_patch(f, 0, 0);
        if (r != FAT_OK)
            return r;
        f->dirent.first_cluster = 0;
        head = 0;
    }

    // extend by whole zeroed clusters; on any failure roll the appended
    // part back -- the dirent size is only patched after the extension is
    // fully in place, so the file stays at `old` bytes throughout
    uint32_t tail = 0;    // current last cluster of the chain
    if (head != 0) {
        r = chain_step(ctx, head, have - 1, &tail);
        if (r != FAT_OK)
            return r;
    }
    uint32_t grown_head = 0; // first cluster allocated below
    uint32_t prev = tail;
    r = FAT_OK;
    for (uint32_t i = 0; i < extra; i++) {
        uint32_t nc;
        r = fat_alloc_cluster(ctx, &nc);
        if (r != FAT_OK)
            break;
        if (prev == 0)
            grown_head = nc; // the chain's new head (empty file grew)
        uint64_t coff;
        if (!cluster_offset(ctx, nc, &coff)) {
            fat_free_chain(ctx, nc);
            if (grown_head == nc)
                grown_head = 0;
            r = FAT_ERR_INVALID_BPB;
            break;
        }
        r = write_zeros(ctx, coff, cluster_size);
        if (r != FAT_OK) {
            fat_free_chain(ctx, nc);
            if (grown_head == nc)
                grown_head = 0;
            break;
        }
        if (prev != 0) {
            r = fat_set_fat_entry(ctx, prev, nc);
            if (r != FAT_OK) {
                fat_free_chain(ctx, nc);
                break; // nc leaks (EOC-marked); the chain stays consistent
            }
        }
        prev = nc;
    }
    if (r != FAT_OK) {
        // roll back: unlink the appended part, then free it
        if (grown_head != 0) {
            if (tail != 0)
                fat_set_fat_entry(ctx, tail, fat_eoc_const(ctx));
            fat_free_chain(ctx, grown_head);
        }
        return r;
    }

    uint32_t new_head = head != 0 ? head : grown_head;
    r = dirent_slot_patch(f, new_head, (uint32_t)size);
    if (r != FAT_OK)
        return r;
    f->dirent.first_cluster = new_head;
    f->dirent.file_size = (uint32_t)size;
    f->size = size;
    return file_cursor_resync(f);
}

fat_result_t fat_file_write(fat_file_t* f, const uint8_t* buf, size_t len,
                            size_t* written) {
    if (f == NULL || buf == NULL || written == NULL)
        return FAT_ERR_INVALID_ARG;
    *written = 0;
    if (!f->writable)
        return FAT_ERR_INVALID_ARG; // read cursors cannot write
    if (len == 0)
        return FAT_OK; // clean no-op (the fat_file_read rule)

    fat_ctx_t* ctx = f->ctx;
    uint32_t cluster_size = fat_cluster_size(ctx);

    uint64_t pos = f->pos;
    uint32_t cluster = f->cluster;
    uint32_t index = f->index;
    size_t done = 0;
    fat_result_t err = FAT_OK;

    if (f->size > 0 && pos == f->size) {
        // EOF entry: the cursor's cluster is only valid while pos < size
        // (seek to EOF does not re-walk), so find the chain's last cluster
        // the way a seek inside the file would
        index = (uint32_t)((f->size - 1) / cluster_size);
        err = chain_step(ctx, f->dirent.first_cluster, index, &cluster);
        if (err != FAT_OK)
            return err;
    }

    while (done < len) {
        uint32_t offset_in = (uint32_t)(pos % cluster_size);
        bool in_file = pos < f->size;
        if (!in_file && offset_in == 0) {
            // at EOF on a cluster boundary: a fresh cluster is needed
            // (writes never create holes: EOF mid-cluster keeps filling
            // the current cluster's tail first)
            uint32_t nc;
            err = fat_alloc_cluster(ctx, &nc);
            if (err != FAT_OK)
                break;
            if (f->dirent.first_cluster == 0) {
                f->dirent.first_cluster = nc; // the chain's new head
                cluster = nc;
                index = 0;
            } else {
                err = fat_set_fat_entry(ctx, cluster, nc);
                if (err != FAT_OK)
                    break; // nc leaks (EOC-marked); the file stays whole
                cluster = nc;
                index++;
            }
        }
        uint64_t coff;
        if (!cluster_offset(ctx, cluster, &coff)) {
            err = FAT_ERR_BAD_CLUSTER;
            break;
        }
        uint64_t n = cluster_size - offset_in;
        if (n > len - done)
            n = len - done;
        err = fat_io_write(ctx, coff + offset_in, buf + done, (size_t)n);
        if (err != FAT_OK)
            break;
        done += (size_t)n;
        pos += n;
        if (pos > f->size)
            f->size = pos; // extension lands with its bytes
        if (done == len)
            break;
        if (pos < f->size) {
            // cluster exhausted inside the file: follow the FAT with
            // fat_file_read's guards
            uint32_t fat;
            err = fat_raw_fat_entry(ctx, cluster, &fat);
            if (err == FAT_ERR_IO)
                break;
            if (err != FAT_OK || fat_is_bad(ctx, fat) ||
                fat_is_eoc(ctx, fat) || fat == 0 ||
                fat_is_reserved(ctx, fat)) {
                if (err == FAT_OK)
                    err = FAT_ERR_BAD_CLUSTER;
                break;
            }
            cluster = fat;
            index++;
        }
        // else: pos == size -- the next iteration allocates
    }

    // commit whatever landed -- on success and failure alike (the prefix
    // consistency contract: *written bytes, dirent size included)
    fat_result_t pr =
        dirent_slot_patch(f, f->dirent.first_cluster, (uint32_t)f->size);
    f->dirent.file_size = (uint32_t)f->size;
    f->pos = pos;
    f->cluster = cluster;
    f->index = index;
    *written = done;
    if (err != FAT_OK)
        return err;
    return pr;
}

// error strings ------------------------------------------------------------

// indexed by fat_result_t (the enum is contiguous and fully covered);
// unknown values fall through to the sentinel tail entry
static const char* const fat_errstr[] = {
    [FAT_OK] = "ok",
    [FAT_ERR_IO] = "I/O error",
    [FAT_ERR_NOMEM] = "out of memory",
    [FAT_ERR_INVALID_BPB] = "invalid BPB",
    [FAT_ERR_UNSUPPORTED] = "unsupported FAT type",
    [FAT_ERR_NAME_TOO_LONG] = "name too long",
    [FAT_ERR_BUFFER_TOO_SMALL] = "buffer too small",
    [FAT_ERR_INVALID_ARG] = "invalid argument",
    [FAT_ERR_NOT_FOUND] = "not found",
    [FAT_ERR_PATH_NOT_FOUND] = "path not found",
    [FAT_ERR_BAD_CLUSTER] = "broken cluster chain",
    [FAT_ERR_END_OF_DIR] = "end of directory",
    [FAT_ERR_DISK_FULL] = "no free cluster",
    [FAT_ERR_DIR_FULL] = "directory full",
    [FAT_ERR_EXISTS] = "entry already exists",
    [FAT_ERR_DIR_NOT_EMPTY] = "directory not empty",
};

const char* fat_strerror(fat_result_t r) {
    size_t i = (size_t)r;
    size_t n = sizeof(fat_errstr) / sizeof(fat_errstr[0]);
    return i < n && fat_errstr[i] != NULL ? fat_errstr[i] : "unknown error";
}
