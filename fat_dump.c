// fat_dump.c -- human-facing dump views (BPB, FAT table, directory tree).
// The only file on the library side that prints; keeps the color.h dep.

#include "fat_internal.h"
#include "color.h"
#include <stdio.h>

// view state (cycled legend colors), shared by the printers below
static int fat_print_color;

static void increment_color() {
    fat_print_color += 1;
    if (fat_print_color >= CL_GRAY + 1)
        fat_print_color = CL_RED;
}

// byte offset of data cluster `cluster` in the image, 0 when out of range
static uint64_t cluster_addr(const fat_ctx_t* ctx, uint32_t cluster) {
    if (ctx == NULL)
        return 0;
    const fat_geometry_t* geo = fat_geometry(ctx);
    if (cluster < 2 || cluster >= 2 + geo->cluster_count)
        return 0;
    // 64-bit intermediates: FAT32 cluster offsets can exceed 32 bits
    return ((uint64_t)geo->data_start_sector +
            (uint64_t)(cluster - 2) * geo->sectors_per_cluster) *
           geo->bytes_per_sector;
}

// pretty-lister internals (defined after the public entry points)
static void fat_print_directory_entry_directory(const fat_ctx_t* ctx,
                                                const fat_dirent_t* entry,
                                                const uint8_t* raw32,
                                                bool recursive);
static void fat_print_directory_entry_file(const fat_ctx_t* ctx,
                                           const fat_dirent_t* entry,
                                           const uint8_t* raw32);

static const char* fat_type_name(const fat_ctx_t* ctx) {
    switch (fat_get_type(ctx)) {
    case FT_FAT12:
        return "FAT12";
    case FT_FAT16:
        return "FAT16";
    case FT_FAT32:
        return "FAT32";
    default:
        return "unknown";
    }
}

void fat_print_info(const fat_ctx_t* ctx) {
    const fat_geometry_t* geo = fat_geometry(ctx);
    if (geo == NULL)
        return;
    printf("fat type: %s\n", fat_type_name(ctx));
    printf("bytesPerSector: %u\n", geo->bytes_per_sector);
    printf("sectorsPerCluster: %u\n", geo->sectors_per_cluster);
    // 1st FAT table's sector
    printf("reservedSectorCount: %u\n", geo->reserved_sectors);
    // count of FAT tables
    printf("tableCount: %u\n", geo->fat_count);
    printf("rootEntryCount: %u\n", geo->root_entries);
    printf("root dir sector count: %u\n", geo->root_dir_sectors);
    printf("total_sectors: %u\n", geo->total_sectors);
    printf("total bytes: %llu\n",
           (unsigned long long)(uint64_t)geo->total_sectors *
               geo->bytes_per_sector);
    // count of FAT table sectors
    printf("tableSize16: %u\n", geo->fat_sectors);

    printf("* fat start_sector %u\n", geo->fat_start_sector);
    printf("* fat sectors %u\n",
           (uint32_t)geo->fat_sectors * geo->fat_count);
    printf("* root_dir start_sector %u\n", geo->root_dir_sector);
    printf("* root_dir sectors %u\n", geo->root_dir_sectors);
    if (fat_get_type(ctx) == FT_FAT32) {
        // the FAT32 root is a cluster chain, not a fixed region
        printf("* root cluster %u\n", geo->root_cluster);
        fat_fsinfo_t info;
        if (fat_fsinfo(ctx, &info) == FAT_OK &&
            info.free_cluster_count != 0xFFFFFFFF)
            printf("* free cluster count: %u\n", info.free_cluster_count);
        else
            printf("* free cluster count: unknown\n");
    }
    printf("* data start_sector %u\n", geo->data_start_sector);
    printf("* data cluster count %u\n", geo->cluster_count);
}

static void fat_print_legend(const char* legend) {
    cl(fat_print_color);
    printf("- %s\n", legend);
    increment_color();
}

static void fat_print_idx(const uint8_t* base, int* idx, const int len) {
    cl(fat_print_color);
    for (int i = 0; i < len; i++) {
        printf("%02x ", (base)[*idx + i]);
    }
    *idx = *idx + len;
    increment_color();
}

static void fat_print_idxstr(const uint8_t* base, int* idxStr,
                             const int len) {
    cl(fat_print_color);
    for (int i = 0; i < len; i++) {
        uint8_t u = base[*idxStr + i];
        if (40 <= u && u <= 126)
            printf("%c ", u);
        else
            printf("**");
    }
    *idxStr = *idxStr + len;
    increment_color();
}

static void fat_print_idx_wide(const uint8_t* base, int* idx,
                               const int* lens) {
    int idxStr = *idx;
    fat_print_color = CL_RED;
    for (int i = 0; lens[i] > 0; i++) {
        fat_print_idx(base, idx, lens[i]);
    }

    fat_print_color = CL_RED;
    for (int i = 0; lens[i] > 0; i++) {
        fat_print_idxstr(base, &idxStr, lens[i]);
    }
}

void fat_print_header_legend() {
    clcl();

    fat_print_color = CL_RED;
    fat_print_legend("relative jump (eb3c) + nop (90)");
    fat_print_legend("OEM Name");
    fat_print_legend("bytes per sector");
    fat_print_legend("sectors per cluster");
    fat_print_legend("FAT table's 1st sector (reserved sectors)");
    fat_print_legend("FAT table count");
    fat_print_legend("Max entries in root table");
    fat_print_legend("Total sector count");
    fat_print_legend("Media type");
    fat_print_legend("sectors per FAT table");
    fat_print_legend("sectors per track");
    fat_print_legend("head count");

    clcl();
}

void fat_print_header_dump(const fat_ctx_t* ctx) {
    clcl();

    const FatBS* bs = (const FatBS*)fat_region_ptr(ctx, 0);
    if (bs == NULL)
        return;

    // 1st line
    int idx = 0;
    const int l[] = {3, 8, 2, 1, 2, -1};
    fat_print_idx_wide((const uint8_t*)bs, &idx, l);
    printf("\n");

    // 2nd line
    idx = 16;
    const int l2[] = {1, 2, 2, 1, 2, 2, 2, 4, -1};
    fat_print_idx_wide((const uint8_t*)bs, &idx, l2);
    printf("\n");

    // 3rd line, FAT32 only: totalSectors32 (offset 32) then the FatExtBS32
    // fields (fatsz32, extFlags, fsVer, rootClus, fsInfo, bkBootSec)
    if (fat_get_type(ctx) == FT_FAT32) {
        idx = 32;
        const int l3[] = {4, 4, 2, 2, 4, 2, 2, -1};
        fat_print_idx_wide((const uint8_t*)bs, &idx, l3);
        printf("\n");
    }

    clcl();
}

void fat_print_fat(const fat_ctx_t* ctx) {
    if (ctx == NULL)
        return;
    // only print the FAT table the context reads (FAT #0, or the active
    // FAT32 table)
    size_t fat_bytes = (size_t)ctx->geo.fat_sectors * ctx->geo.bytes_per_sector;
    const char* fmt;
    uint32_t entryCount;
    uint32_t maxShow; // cap for FAT32: a full table is unusable output
    switch (ctx->type) {
    case FT_FAT16:
        fmt = "%04X ";
        entryCount = (uint32_t)(fat_bytes / 2);
        maxShow = 0xFFFFFFFFu;
        break;
    case FT_FAT32:
        fmt = "%08X ";
        entryCount = (uint32_t)(fat_bytes / 4);
        maxShow = 1024;
        break;
    default: // FAT12 packs two 12-bit entries into every 3 bytes
        fmt = "%03X ";
        entryCount = (uint32_t)(fat_bytes * 2 / 3);
        maxShow = 0xFFFFFFFFu;
        break;
    }
    uint32_t shown = 0;
    bool truncated = false;
    for (uint32_t i = 0; i < entryCount; i++) {
        uint32_t value = fat_raw_fat_entry(ctx, i);
        if (value == FAT_CLUSTER_NOT_FOUND)
            break; // FAT region ended
        printf(fmt, value);
        if (i % 10 == 9)
            printf("\n");
        shown++;
        if (shown == maxShow) {
            truncated = i + 1 < entryCount;
            break;
        }
    }
    printf("\n");
    if (truncated)
        printf("... (%u more entries truncated)\n", entryCount - shown);
}

void fat_print_directory_entry_header_legend() {
    clcl();

    fat_print_color = CL_RED;
    fat_print_legend("Name");
    fat_print_legend("Attributes");
    fat_print_legend("(reserved)");
    fat_print_legend("Creation time (millisec)");
    fat_print_legend("Creation time");
    fat_print_legend("Creation date");
    fat_print_legend("Last access date");
    fat_print_legend("(ignored in FAT12)");
    fat_print_legend("Last write time");
    fat_print_legend("Last write date");
    fat_print_legend("Starting cluster");
    fat_print_legend("File size");

    clcl();
}

void fat_print_directory_entry_dump(const fat_dirent_t* entry,
                                    const uint8_t* raw32, void* user_data) {
    (void)entry;     // parsed view unused; the dump shows the raw bytes
    (void)user_data; // kept for the callback signature

    // 1st line
    int idx = 0;
    const int l1[] = {11, 1, 1, 1, 2, -1};
    const int l2[] = {2, 2, 2, 2, 2, 2, 4, -1};
    fat_print_idx_wide(raw32, &idx, l1);
    printf("\n");
    // 2nd line
    fat_print_idx_wide(raw32, &idx, l2);
    printf("\n");
    clcl();
}

// adapter: feed iter_dir's (entry, raw32) pairs back into the pretty-lister
static void iter_print_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                          void* user_data);

static void print_entry_full(const fat_ctx_t* ctx, const fat_dirent_t* entry,
                             const uint8_t* raw32);

// pretty-lister entry point for already-parsed entries (no raw bytes at
// hand: the byte dump is skipped)
void fat_print_directory_entry(const fat_ctx_t* ctx,
                               const fat_dirent_t* entry) {
    print_entry_full(ctx, entry, NULL);
}

static void print_entry_full(const fat_ctx_t* ctx, const fat_dirent_t* entry,
                             const uint8_t* raw32) {
    // Check if entry is unused or deleted (defensive: iter_dir filters
    // these already)
    if (entry->name[0] == '\0')
        return;

    clcl();

    // Check if entry is a directory
    if (entry->attributes & ATTR_DIRECTORY) {
        // Directory ("." / ".." stay silent)
        if (entry->name[0] != '.')
            fat_print_directory_entry_directory(ctx, entry, raw32, true);
    } else {
        // File entry
        fat_print_directory_entry_file(ctx, entry, raw32);
    }
}

static void iter_print_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                          void* user_data) {
    print_entry_full((const fat_ctx_t*)user_data, entry, raw32);
}

static void fat_print_directory_entry_directory(const fat_ctx_t* ctx,
                                                const fat_dirent_t* entry,
                                                const uint8_t* raw32,
                                                bool recursive) {
    // the entry must be a directory
    printf("Directory: %s, cluster:%u[0x%08llX]\n", entry->name,
           entry->first_cluster,
           (unsigned long long)cluster_addr(ctx, entry->first_cluster));
    if (raw32 != NULL)
        fat_print_directory_entry_dump(entry, raw32, NULL);

    if (recursive) {
        // walk the sub directory's cluster chain; iter_dir caps the walk
        // and skips deleted/LFN entries
        (void)fat_iter_dir((fat_ctx_t*)ctx, entry->first_cluster,
                           iter_print_cb, (void*)ctx);
    }
}

static void fat_print_directory_entry_file(const fat_ctx_t* ctx,
                                           const fat_dirent_t* entry,
                                           const uint8_t* raw32) {
    // the entry must be a file
    printf("File: %s, cluster:%u[0x%08llX],  size:%u\n", entry->name,
           entry->first_cluster,
           (unsigned long long)cluster_addr(ctx, entry->first_cluster),
           entry->file_size);
    if (raw32 != NULL)
        fat_print_directory_entry_dump(entry, raw32, NULL);
}
