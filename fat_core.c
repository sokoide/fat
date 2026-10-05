// fat_core.c -- context lifecycle, BPB validation, FAT12/16/32 decode,
// directory iteration and path lookup. No stdio, no color: errors are
// fat_result_t.

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

const uint8_t* fat_region_ptr(const fat_ctx_t* ctx, size_t offset) {
    if (ctx == NULL || offset > ctx->image_size)
        return NULL;
    return ctx->image + offset;
}

const uint8_t* fat_fat_ptr(const fat_ctx_t* ctx) {
    if (ctx == NULL)
        return NULL;
    size_t fat_offset = (size_t)ctx->geo.fat_start_sector *
                        ctx->geo.bytes_per_sector;
    size_t fat_bytes = (size_t)ctx->geo.fat_sectors *
                       ctx->geo.bytes_per_sector;
    if (fat_offset > ctx->image_size ||
        fat_bytes > ctx->image_size - fat_offset)
        return NULL;
    return ctx->image + fat_offset;
}

uint32_t fat_raw_fat_entry(const fat_ctx_t* ctx, uint32_t cluster) {
    const uint8_t* fatp = fat_fat_ptr(ctx);
    if (fatp == NULL)
        return FAT_CLUSTER_NOT_FOUND;

    // byte span of entry `cluster` per packing (FAT12 packs two 12-bit
    // entries into every 3 bytes)
    size_t fat_bytes = (size_t)ctx->geo.fat_sectors *
                       ctx->geo.bytes_per_sector;
    size_t need;
    switch (ctx->type) {
    case FT_FAT16:
        need = (size_t)cluster * 2 + 2;
        break;
    case FT_FAT32:
        need = (size_t)cluster * 4 + 4;
        break;
    case FT_FAT12:
        need = (size_t)(cluster / 2) * 3 + 3;
        break;
    default:
        return FAT_CLUSTER_NOT_FOUND;
    }
    if (need > fat_bytes)
        return FAT_CLUSTER_NOT_FOUND;

    switch (ctx->type) {
    case FT_FAT16:
        return rd16(fatp + (size_t)cluster * 2);
    case FT_FAT32:
        // the upper 4 bits are reserved flags; masking is mandatory for
        // EOC detection
        return rd32(fatp + (size_t)cluster * 4) & 0x0FFFFFFF;
    case FT_FAT12: {
        uint32_t value;
        const uint8_t* e = &fatp[(size_t)(cluster / 2) * 3];
        if (cluster % 2 == 0) {
            value = e[0];
            value |= (e[1] & 0x0F) << 8;
        } else {
            value = e[2];
            value <<= 4;
            value |= (e[1] & 0xF0) >> 4;
        }
        return value;
    }
    default:
        return FAT_CLUSTER_NOT_FOUND;
    }
}

// EOC / bad / reserved classification of a raw FAT value, per image type
// (thresholds: FAT12 0xFF0/0xFF7/0xFF8, FAT16 0xFFF0/0xFFF7/0xFFF8,
// FAT32 0x0FFFFFF0/0x0FFFFFF7/0x0FFFFFF8 after masking)
static bool fat_is_bad(const fat_ctx_t* ctx, uint32_t entry) {
    switch (ctx->type) {
    case FT_FAT16:
        return entry == 0xFFF7;
    case FT_FAT32:
        return entry == 0x0FFFFFF7;
    case FT_FAT12:
        return entry == 0xFF7;
    default:
        return true;
    }
}

static bool fat_is_eoc(const fat_ctx_t* ctx, uint32_t entry) {
    switch (ctx->type) {
    case FT_FAT16:
        return entry >= 0xFFF8;
    case FT_FAT32:
        return entry >= 0x0FFFFFF8;
    case FT_FAT12:
        return entry >= 0xFF8;
    default:
        return false;
    }
}

// reserved values (e.g. FAT12 0xFF0..0xFF6) must never be followed as
// cluster numbers
static bool fat_is_reserved(const fat_ctx_t* ctx, uint32_t entry) {
    switch (ctx->type) {
    case FT_FAT16:
        return entry >= 0xFFF0 && entry < 0xFFF7;
    case FT_FAT32:
        return entry >= 0x0FFFFFF0 && entry < 0x0FFFFFF7;
    case FT_FAT12:
        return entry >= 0xFF0 && entry < 0xFF7;
    default:
        return true;
    }
}

// fill a public fat_dirent_t from 32 raw on-disk bytes
static void dirent_from_raw(const fat_ctx_t* ctx, const uint8_t* raw32,
                            fat_dirent_t* out) {
    const DirectoryEntry* e = (const DirectoryEntry*)raw32;
    fat_name_from_83(e->name, e->attributes, out->name, FAT_NAME_MAX);
    out->attributes = e->attributes;
    out->creation_time_tenth = e->creationTimeTenthOfSecond;
    out->creation_time = e->creationTime;
    out->creation_date = e->creationDate;
    out->last_access_date = e->lastAccessDate;
    out->last_write_time = e->lastWriteTime;
    out->last_write_date = e->lastWriteDate;
    // FAT32 spreads the first cluster over two 16-bit halves; FAT12/16 use
    // the low half only
    if (ctx->type == FT_FAT32)
        out->first_cluster = ((uint32_t)e->firstClusterHigh << 16) |
                             e->firstClusterLow;
    else
        out->first_cluster = e->firstClusterLow;
    out->file_size = e->fileSize;
}

// pointer to data cluster `cluster`, or NULL when out of range
static const uint8_t* cluster_ptr(const fat_ctx_t* ctx, uint32_t cluster) {
    if (cluster < 2 || cluster >= 2 + ctx->geo.cluster_count)
        return NULL;
    // 64-bit intermediates: cluster * sectors_per_cluster * bytes_per_sector
    // can exceed 32 bits in hostile BPBs
    uint64_t offset = ((uint64_t)ctx->geo.data_start_sector +
                       (uint64_t)(cluster - 2) * ctx->geo.sectors_per_cluster) *
                      ctx->geo.bytes_per_sector;
    if (offset + fat_cluster_size(ctx) > (uint64_t)ctx->image_size)
        return NULL;
    return ctx->image + (size_t)offset;
}

// shared BPB validation + image copy; fat_open (fat_dev.c) and fat_open_mem
// both land here
fat_result_t fat_ctx_init_mem(fat_ctx_t** out, const uint8_t* image,
                              size_t size) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    if (image == NULL)
        return FAT_ERR_INVALID_ARG;
    if (size < 512)
        return FAT_ERR_INVALID_BPB;

    // validate the BPB before trusting any of its values
    if (image[510] != 0x55 || image[511] != 0xAA)
        return FAT_ERR_INVALID_BPB;
    const FatBS* bs = (const FatBS*)image;
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
    if (type == FT_FAT32) {
        uint16_t extFlags = rd16((const uint8_t*)&ext32->extFlags);
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

    // the whole volume must actually fit in the buffer
    uint64_t imageSize = totSec * bytesPerSector;
    if (imageSize > (uint64_t)size)
        return FAT_ERR_INVALID_BPB;

    fat_ctx_t* ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
        return FAT_ERR_NOMEM;
    ctx->image = malloc((size_t)imageSize);
    if (ctx->image == NULL) {
        free(ctx);
        return FAT_ERR_NOMEM;
    }
    memcpy(ctx->image, image, (size_t)imageSize);
    ctx->image_size = (size_t)imageSize;
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

    *out = ctx;
    return FAT_OK;
}

// lifecycle --------------------------------------------------------------

fat_result_t fat_open_mem(const uint8_t* image, size_t size,
                          fat_ctx_t** out) {
    return fat_ctx_init_mem(out, image, size);
}

void fat_close(fat_ctx_t* ctx) {
    if (ctx == NULL)
        return;
    free(ctx->image);
    free(ctx);
}

// introspection ----------------------------------------------------------

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
    uint32_t value = fat_raw_fat_entry(ctx, cluster);
    if (value == FAT_CLUSTER_NOT_FOUND)
        return FAT_ERR_INVALID_BPB; // FAT region does not cover the index
    *out = value;
    return FAT_OK;
}

// directory iteration ----------------------------------------------------

// the 11-byte on-disk encodings of the dot entries
static const uint8_t fat_dot11[11] = {'.', ' ', ' ', ' ', ' ', ' ',
                                      ' ', ' ', ' ', ' ', ' '};
static const uint8_t fat_dotdot11[11] = {'.', '.', ' ', ' ', ' ', ' ',
                                         ' ', ' ', ' ', ' ', ' '};

// stepwise iterator over one directory; the single scan implementation
// behind both fat_dir_next and fat_iter_dir
struct fat_dir {
    fat_ctx_t* ctx;
    bool fixed;         // FAT12/16 fixed root region (not a cluster chain)
    bool done;          // sticky exhaustion (further next calls stay at END)
    uint32_t root_next; // fixed mode: next slot index in the root region
    uint32_t cluster;   // chain mode: current cluster
    uint32_t slot;      // chain mode: next entry slot within the cluster
    uint32_t visited;   // chain mode: clusters entered so far (loop cap)
    uint32_t fat_next;  // chain mode: FAT value of the current cluster
    const uint8_t* cluster_data; // chain mode: pointer into the image
    fat_dirent_t entry; // cursor-owned storage handed to the caller
    uint8_t raw[32];    // ditto; valid until the next fat_dir_next/close
};

// validate one cluster of a directory chain: in-image and not marked bad
static fat_result_t dir_cluster_enter(fat_ctx_t* ctx, uint32_t cluster,
                                      uint32_t* fat_next,
                                      const uint8_t** data) {
    const uint8_t* p = cluster_ptr(ctx, cluster);
    if (p == NULL)
        return FAT_ERR_BAD_CLUSTER;
    uint32_t fat = fat_raw_fat_entry(ctx, cluster);
    if (fat == FAT_CLUSTER_NOT_FOUND || fat_is_bad(ctx, fat))
        return FAT_ERR_BAD_CLUSTER;
    *fat_next = fat;
    *data = p;
    return FAT_OK;
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
        size_t root_offset = (size_t)ctx->geo.root_dir_sector *
                             ctx->geo.bytes_per_sector;
        if (fat_region_ptr(ctx, root_offset) == NULL) {
            free(d);
            return FAT_ERR_INVALID_BPB;
        }
        d->fixed = true;
        *out = d;
        return FAT_OK;
    }

    // chain mode: the FAT32 root (sentinel or its real cluster number) or
    // any subdirectory cluster
    if (dir_cluster == FAT_CLUSTER_ROOT)
        dir_cluster = ctx->geo.root_cluster;

    if (dir_cluster < 2 || dir_cluster >= 2 + ctx->geo.cluster_count) {
        free(d);
        return FAT_ERR_INVALID_ARG;
    }

    // DOS invariant: every subdirectory starts with a "." self-entry (first
    // 32 bytes: '.' + 10 spaces, ATTR_DIRECTORY); data or garbage here is
    // not a directory. The root is exempt -- neither the fixed region nor
    // the FAT32 root chain carries dot entries.
    if (!(ctx->type == FT_FAT32 && dir_cluster == ctx->geo.root_cluster)) {
        const uint8_t* p = cluster_ptr(ctx, dir_cluster);
        if (p == NULL || memcmp(p, fat_dot11, 11) != 0 ||
            (p[11] & ATTR_DIRECTORY) == 0) {
            free(d);
            return FAT_ERR_INVALID_ARG;
        }
    }

    fat_result_t r =
        dir_cluster_enter(ctx, dir_cluster, &d->fat_next, &d->cluster_data);
    if (r != FAT_OK) {
        free(d);
        return r;
    }
    d->cluster = dir_cluster;
    d->visited = 1;
    *out = d;
    return FAT_OK;
}

fat_result_t fat_dir_next(fat_dir_t* d, const fat_dirent_t** out,
                          const uint8_t** raw32) {
    if (d == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    if (d->done)
        return FAT_ERR_END_OF_DIR; // sticky exhaustion

    if (d->fixed) {
        const fat_ctx_t* ctx = d->ctx;
        size_t root_offset = (size_t)ctx->geo.root_dir_sector *
                             ctx->geo.bytes_per_sector;
        const uint8_t* p = fat_region_ptr(ctx, root_offset);
        if (p == NULL)
            return FAT_ERR_INVALID_BPB;
        while (d->root_next < ctx->geo.root_entries) {
            const uint8_t* raw =
                p + (size_t)d->root_next++ * sizeof(DirectoryEntry);
            // 0x00: no more entries, 0xE5: deleted
            if (raw[0] == 0x00)
                break;
            if (raw[0] == 0xE5)
                continue;
            if (raw[11] == ATTR_LONG_NAME)
                continue; // long file name entry
            memcpy(d->raw, raw, sizeof(d->raw));
            dirent_from_raw(d->ctx, d->raw, &d->entry);
            *out = &d->entry;
            if (raw32 != NULL)
                *raw32 = d->raw;
            return FAT_OK;
        }
        d->done = true;
        return FAT_ERR_END_OF_DIR;
    }

    uint32_t entriesPerCluster =
        fat_cluster_size(d->ctx) / (uint32_t)sizeof(DirectoryEntry);
    for (;;) {
        bool end_of_dir = false;
        while (d->slot < entriesPerCluster) {
            const uint8_t* raw =
                d->cluster_data + (size_t)d->slot++ * sizeof(DirectoryEntry);
            // 0x00: no more entries, 0xE5: deleted
            if (raw[0] == 0x00) {
                end_of_dir = true;
                break;
            }
            if (raw[0] == 0xE5)
                continue;
            if (raw[11] == ATTR_LONG_NAME)
                continue; // long file name entry
            memcpy(d->raw, raw, sizeof(d->raw));
            dirent_from_raw(d->ctx, d->raw, &d->entry);
            *out = &d->entry;
            if (raw32 != NULL)
                *raw32 = d->raw;
            return FAT_OK;
        }
        if (end_of_dir)
            break;

        // this cluster is exhausted: follow the chain with the same guards
        // as the file walkers (bad/free/reserved, bounds, visited cap)
        uint32_t fat = d->fat_next;
        if (fat_is_eoc(d->ctx, fat))
            break; // natural end of the directory
        if (fat == 0 || fat_is_reserved(d->ctx, fat))
            return FAT_ERR_BAD_CLUSTER; // free/reserved while chain continues
        if (d->visited > d->ctx->geo.cluster_count)
            return FAT_ERR_BAD_CLUSTER; // longer than the image: a loop
        uint32_t next = fat;
        if (next < 2 || next >= 2 + d->ctx->geo.cluster_count)
            return FAT_ERR_BAD_CLUSTER;
        fat_result_t r =
            dir_cluster_enter(d->ctx, next, &d->fat_next, &d->cluster_data);
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

fat_result_t fat_iter_dir(fat_ctx_t* ctx, uint32_t dir_cluster,
                          fat_iter_cb cb, void* user_data) {
    if (ctx == NULL || cb == NULL)
        return FAT_ERR_INVALID_ARG;

    // thin loop over the cursor: one scan implementation, no duplicated
    // chain/root logic
    fat_dir_t* d = NULL;
    fat_result_t r = fat_dir_open(ctx, dir_cluster, &d);
    if (r != FAT_OK)
        return r;
    const fat_dirent_t* entry;
    const uint8_t* raw32;
    while ((r = fat_dir_next(d, &entry, &raw32)) == FAT_OK)
        cb(entry, raw32, user_data);
    fat_dir_close(d);
    return r == FAT_ERR_END_OF_DIR ? FAT_OK : r;
}

// 8.3 name conversion ----------------------------------------------------

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

    size_t len = baseLen;
    // directories and volume labels get no extension dot; a file with an
    // all-blank extension gets none either
    if ((attributes & (ATTR_DIRECTORY | ATTR_VOLUME_ID)) == 0) {
        uint32_t extLen = 3;
        while (extLen > 0 && render[8 + extLen - 1] == 0x20)
            extLen--;
        if (extLen > 0)
            len += 1 + extLen;
    }

    if (len + 1 > out_len)
        return FAT_ERR_BUFFER_TOO_SMALL;

    memcpy(out, render, baseLen);
    size_t o = baseLen;
    if (len > baseLen) {
        uint32_t extLen = 3;
        while (extLen > 0 && render[8 + extLen - 1] == 0x20)
            extLen--;
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

// timestamps -------------------------------------------------------------

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

// FSInfo (FAT32 only) ------------------------------------------------------

fat_result_t fat_fsinfo(const fat_ctx_t* ctx, fat_fsinfo_t* out) {
    if (ctx == NULL || out == NULL)
        return FAT_ERR_INVALID_ARG;
    out->free_cluster_count = 0xFFFFFFFF; // unknown/stale
    out->next_free_cluster = 0xFFFFFFFF;
    if (ctx->type != FT_FAT32)
        return FAT_ERR_UNSUPPORTED;

    // 0 = no FSInfo sector; otherwise the signatures decide validity
    if (ctx->fsinfo_sector == 0)
        return FAT_OK;
    uint64_t offset = (uint64_t)ctx->fsinfo_sector * ctx->geo.bytes_per_sector;
    const uint8_t* p = fat_region_ptr(ctx, (size_t)offset);
    if (p == NULL || ctx->image_size - (size_t)offset < 512)
        return FAT_OK; // sector absent from the image
    if (rd32(p) != 0x41615252 ||       // "RRaA"
        rd32(p + 484) != 0x61417272 || // "rrAa"
        rd32(p + 508) != 0xAA550000)
        return FAT_OK; // invalid signature: report unknown
    out->free_cluster_count = rd32(p + 488);
    out->next_free_cluster = rd32(p + 492);
    return FAT_OK;
}

// path lookup ------------------------------------------------------------

typedef struct {
    const uint8_t* name11; // 11-byte on-disk name to match
    fat_dirent_t hit;      // filled on match
    bool found;
} LookupArg;

static void lookup_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                      void* user_data) {
    // volume labels are metadata, not openable objects: never match a
    // lookup, like DOS open() (ATTR_LONG_NAME carries the same bit, but
    // LFN entries are filtered out during iteration already)
    if (raw32[11] & ATTR_VOLUME_ID)
        return;
    LookupArg* arg = (LookupArg*)user_data;
    if (!arg->found && memcmp(arg->name11, raw32, 11) == 0) {
        arg->found = true;
        arg->hit = *entry;
    }
}

// search one directory for the 11-byte name; false when absent
static bool lookup_in_dir(fat_ctx_t* ctx, uint32_t dir_cluster,
                          const uint8_t name11[11], fat_dirent_t* out) {
    LookupArg arg;
    arg.name11 = name11;
    arg.found = false;
    if (fat_iter_dir(ctx, dir_cluster, lookup_cb, &arg) != FAT_OK)
        return false;
    if (!arg.found)
        return false;
    *out = arg.hit;
    return true;
}

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
                if (!lookup_in_dir(ctx, cur, fat_dot11, &self))
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
            if (!lookup_in_dir(ctx, cur, fat_dotdot11, &dotdot))
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

        uint8_t name11[11];
        fat_result_t r = fat_name_to_83(token, name11);
        if (r == FAT_ERR_NAME_TOO_LONG)
            return r;
        if (r != FAT_OK)
            return FAT_ERR_INVALID_ARG;

        fat_dirent_t hit;
        if (!lookup_in_dir(ctx, cur, name11, &hit)) {
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

// file read --------------------------------------------------------------

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
    if (cluster_size == 0)
        return FAT_ERR_INVALID_BPB;

    uint8_t* buf = malloc(file->file_size);
    if (buf == NULL)
        return FAT_ERR_NOMEM;

    // Brent's algorithm anchors; `tortoise` lags behind the advancing head
    uint32_t tortoise = cluster;
    uint32_t power = 1;
    uint32_t lam = 1;

    // chain-read exactly file_size bytes, capped at the total data cluster
    // count (+1) against cycles in a corrupt FAT
    uint32_t remaining = file->file_size;
    uint32_t copied = 0;
    for (uint32_t visited = 0;; visited++) {
        const uint8_t* p = cluster_ptr(ctx, cluster);
        uint32_t fat = p != NULL ? fat_raw_fat_entry(ctx, cluster)
                                 : FAT_CLUSTER_NOT_FOUND;
        if (p == NULL || fat == FAT_CLUSTER_NOT_FOUND || fat_is_bad(ctx, fat))
            break; // broken

        uint32_t n = remaining < cluster_size ? remaining : cluster_size;
        memcpy(buf + copied, p, n);
        copied += n;
        remaining -= n;
        if (remaining == 0) {
            *out = buf;
            *out_size = file->file_size;
            return FAT_OK;
        }

        if (fat_is_eoc(ctx, fat))
            break; // chain ends before file_size
        if (fat == 0 || fat_is_reserved(ctx, fat))
            break; // free/reserved while the file continues
        cluster = fat;
        if (cluster < 2 || cluster >= 2 + ctx->geo.cluster_count ||
            visited >= ctx->geo.cluster_count)
            break; // chain longer than the image can hold

        // A healthy chain never revisits a cluster. The iteration cap above
        // only catches loops that starve the read; a self-pointing entry
        // satisfies file_size with garbage, so detect revisits explicitly
        // (Brent's cycle detection: O(1) space, one extra compare per step).
        if (cluster == tortoise)
            break; // cycle
        if (power == lam) {
            tortoise = cluster;
            power <<= 1;
            lam = 0;
        } else {
            lam++;
        }
    }

    // no partial buffer on error
    free(buf);
    return FAT_ERR_BAD_CLUSTER;
}

// streaming file access ----------------------------------------------------

// read cursor over one file's cluster chain; the dirent is copied at open,
// the context is borrowed (fat_close(ctx) before fat_file_close is a
// caller error)
struct fat_file {
    fat_ctx_t* ctx;
    fat_dirent_t dirent; // copy: callers may pass stack objects
    uint64_t pos;        // absolute byte position, always <= size
    uint64_t size;
    uint32_t cluster;    // cluster holding `pos` (valid while pos < size)
    uint32_t index;      // chain index of `cluster`
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
// bad/free/reserved values, EOC short of the target, bounds, visited cap
// and Brent cycle detection (O(1) space, one extra compare per step)
static fat_result_t chain_step(const fat_ctx_t* ctx, uint32_t first,
                               uint32_t steps, uint32_t* out) {
    uint32_t cluster = first;
    uint32_t tortoise = first;
    uint32_t power = 1;
    uint32_t lam = 1;
    for (uint32_t i = 0; i < steps; i++) {
        uint32_t fat = fat_raw_fat_entry(ctx, cluster);
        if (fat == FAT_CLUSTER_NOT_FOUND || fat_is_bad(ctx, fat) ||
            fat_is_eoc(ctx, fat) || fat == 0 || fat_is_reserved(ctx, fat))
            return FAT_ERR_BAD_CLUSTER;
        cluster = fat;
        if (cluster < 2 || cluster >= 2 + ctx->geo.cluster_count)
            return FAT_ERR_BAD_CLUSTER;
        // a healthy chain never revisits a cluster
        if (cluster == tortoise)
            return FAT_ERR_BAD_CLUSTER; // cycle
        if (power == lam) {
            tortoise = cluster;
            power <<= 1;
            lam = 0;
        } else {
            lam++;
        }
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
    if (cluster_size == 0)
        return FAT_ERR_INVALID_BPB;

    uint8_t* o = buf;
    uint64_t left = want;
    uint32_t cluster = f->cluster;
    uint32_t index = f->index;
    uint32_t offset_in = (uint32_t)(f->pos % cluster_size);

    // Brent anchors for this call's walk (same detection as fat_read_file)
    uint32_t tortoise = cluster;
    uint32_t power = 1;
    uint32_t lam = 1;
    uint32_t steps = 0;

    while (left > 0) {
        const uint8_t* p = cluster_ptr(f->ctx, cluster);
        if (p == NULL)
            return FAT_ERR_BAD_CLUSTER;
        uint32_t avail = cluster_size - offset_in;
        uint64_t n = left < avail ? left : avail;
        memcpy(o, p + offset_in, (size_t)n);
        o += n;
        left -= n;
        offset_in += (uint32_t)n;
        if (left == 0)
            break;

        // need the next cluster: follow the FAT with fat_read_file's guards
        uint32_t fat = fat_raw_fat_entry(f->ctx, cluster);
        if (fat == FAT_CLUSTER_NOT_FOUND || fat_is_bad(f->ctx, fat) ||
            fat_is_eoc(f->ctx, fat) || fat == 0 ||
            fat_is_reserved(f->ctx, fat))
            return FAT_ERR_BAD_CLUSTER; // chain ends before the file does
        cluster = fat;
        index++;
        steps++;
        if (cluster < 2 || cluster >= 2 + f->ctx->geo.cluster_count ||
            steps > f->ctx->geo.cluster_count)
            return FAT_ERR_BAD_CLUSTER; // out of range or chain loop cap
        if (cluster == tortoise)
            return FAT_ERR_BAD_CLUSTER; // cycle
        if (power == lam) {
            tortoise = cluster;
            power <<= 1;
            lam = 0;
        } else {
            lam++;
        }
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

// error strings ----------------------------------------------------------

const char* fat_strerror(fat_result_t r) {
    switch (r) {
    case FAT_OK:
        return "ok";
    case FAT_ERR_IO:
        return "I/O error";
    case FAT_ERR_NOMEM:
        return "out of memory";
    case FAT_ERR_INVALID_BPB:
        return "invalid BPB";
    case FAT_ERR_UNSUPPORTED:
        return "unsupported FAT type";
    case FAT_ERR_NAME_TOO_LONG:
        return "name too long";
    case FAT_ERR_BUFFER_TOO_SMALL:
        return "buffer too small";
    case FAT_ERR_INVALID_ARG:
        return "invalid argument";
    case FAT_ERR_NOT_FOUND:
        return "not found";
    case FAT_ERR_PATH_NOT_FOUND:
        return "path not found";
    case FAT_ERR_BAD_CLUSTER:
        return "broken cluster chain";
    case FAT_ERR_END_OF_DIR:
        return "end of directory";
    }
    return "unknown error";
}
