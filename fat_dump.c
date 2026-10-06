// fat_dump.c -- human-facing dump views (BPB, FAT table, directory tree).
// The only file on the library side that prints; keeps the color.h dep.
// Views never hold image pointers: bytes arrive in local buffers through
// fat_io_read and the public accessors, so they work on any backend.

#include "fat_internal.h"
#include "color.h"
#include <stdio.h>

// legend/byte printers cycle the colors CL_RED..CL_GRAY. The cycle counter
// lives in each caller's frame and is passed as *color: no file-scope
// state, so dump calls on different contexts never share it (reentrant).
static void increment_color(int* color) {
    *color += 1;
    if (*color >= CL_GRAY + 1)
        *color = CL_RED;
}

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

static void fat_print_legend(const char* legend, int* color) {
    cl(*color);
    printf("- %s\n", legend);
    increment_color(color);
}

static void fat_print_idx(const uint8_t* base, int* idx, const int len,
                          int* color) {
    cl(*color);
    for (int i = 0; i < len; i++) {
        printf("%02x ", (base)[*idx + i]);
    }
    *idx = *idx + len;
    increment_color(color);
}

static void fat_print_idxstr(const uint8_t* base, int* idxStr, const int len,
                             int* color) {
    cl(*color);
    for (int i = 0; i < len; i++) {
        uint8_t u = base[*idxStr + i];
        if (40 <= u && u <= 126)
            printf("%c ", u);
        else
            printf("**");
    }
    *idxStr = *idxStr + len;
    increment_color(color);
}

static void fat_print_idx_wide(const uint8_t* base, int* idx, const int* lens) {
    int idxStr = *idx;
    int color = CL_RED;
    for (int i = 0; lens[i] > 0; i++) {
        fat_print_idx(base, idx, lens[i], &color);
    }

    color = CL_RED;
    for (int i = 0; lens[i] > 0; i++) {
        fat_print_idxstr(base, &idxStr, lens[i], &color);
    }
}

void fat_print_header_legend(void) {
    clcl();

    int color = CL_RED;
    fat_print_legend("relative jump (eb3c) + nop (90)", &color);
    fat_print_legend("OEM Name", &color);
    fat_print_legend("bytes per sector", &color);
    fat_print_legend("sectors per cluster", &color);
    fat_print_legend("FAT table's 1st sector (reserved sectors)", &color);
    fat_print_legend("FAT table count", &color);
    fat_print_legend("Max entries in root table", &color);
    fat_print_legend("Total sector count", &color);
    fat_print_legend("Media type", &color);
    fat_print_legend("sectors per FAT table", &color);
    fat_print_legend("sectors per track", &color);
    fat_print_legend("head count", &color);

    clcl();
}

void fat_print_header_dump(const fat_ctx_t* ctx) {
    clcl();

    if (ctx == NULL)
        return;

    // boot sector / EBPB bytes into a local buffer through the I/O layer
    // (no image pointer). The read only fills the sector cache, so it is
    // semantically const -- hence the cast.
    uint8_t bs[512];
    if (fat_io_read((fat_ctx_t*)ctx, 0, bs, sizeof(bs)) != FAT_OK)
        return;

    // 1st line
    int idx = 0;
    const int l[] = {3, 8, 2, 1, 2, -1};
    fat_print_idx_wide(bs, &idx, l);
    printf("\n");

    // 2nd line
    idx = 16;
    const int l2[] = {1, 2, 2, 1, 2, 2, 2, 4, -1};
    fat_print_idx_wide(bs, &idx, l2);
    printf("\n");

    // 3rd line, FAT32 only: totalSectors32 (offset 32) then the FatExtBS32
    // fields (fatsz32, extFlags, fsVer, rootClus, fsInfo, bkBootSec)
    if (fat_get_type(ctx) == FT_FAT32) {
        idx = 32;
        const int l3[] = {4, 4, 2, 2, 4, 2, 2, -1};
        fat_print_idx_wide(bs, &idx, l3);
        printf("\n");
    }

    clcl();
}

void fat_print_fat(const fat_ctx_t* ctx) {
    const fat_geometry_t* geo = fat_geometry(ctx);
    if (geo == NULL)
        return;
    // only print the FAT table the context reads (FAT #0, or the active
    // FAT32 table)
    size_t fat_bytes = (size_t)geo->fat_sectors * geo->bytes_per_sector;
    const char* fmt;
    uint32_t entryCount;
    uint32_t maxShow; // cap for FAT32: a full table is unusable output
    switch (fat_get_type(ctx)) {
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
        uint32_t value;
        // entries are read through the public accessor: indices
        // 0..cluster_count+1 are real, past that the table is sector
        // rounding padding -- stop (the old code broke on region end)
        if (fat_get_fat_entry(ctx, i, &value) != FAT_OK)
            break; // no more real FAT entries
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

void fat_print_directory_entry_header_legend(void) {
    clcl();

    int color = CL_RED;
    fat_print_legend("Name", &color);
    fat_print_legend("Attributes", &color);
    fat_print_legend("(reserved)", &color);
    fat_print_legend("Creation time (millisec)", &color);
    fat_print_legend("Creation time", &color);
    fat_print_legend("Creation date", &color);
    fat_print_legend("Last access date", &color);
    fat_print_legend("(ignored in FAT12)", &color);
    fat_print_legend("Last write time", &color);
    fat_print_legend("Last write date", &color);
    fat_print_legend("Starting cluster", &color);
    fat_print_legend("File size", &color);

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
