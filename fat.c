#include "fat.h"
#include "color.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// globals

// static vars
static int fat_print_color;
static void* _fat_buffer;
static FatBS* _fat_bs;
static enum FAT_TYPE _fat_type;
static uint32_t _fat_start_sector;
static uint32_t _fat_sectors;
static uint32_t _root_dir_start_sector;
static uint32_t _root_dir_sectors;
static uint32_t _data_start_sector;
static uint32_t _data_cluster_count;

// functions
bool fat_init(FILE* fp) {
    uint8_t b[512];
    FatBS* bs = (FatBS*)&b;  // probe only; the globals are set on success
    _fat_type = FT_UNKNOWN;
    fseek(fp, 0, SEEK_SET);

    size_t read = fread(b, sizeof(b) / sizeof(b[0]), 1, fp);
    if (read != 1) {
        fprintf(stderr, "fat_init failed to read the boot sector.\n");
        return false;
    }

    // validate the BPB before trusting any of its values
    if (b[510] != 0x55 || b[511] != 0xAA) {
        fprintf(stderr, "fat_init boot signature 0x55 0xAA not found.\n");
        return false;
    }
    if (bs->bytesPerSector != 512 && bs->bytesPerSector != 1024 &&
        bs->bytesPerSector != 2048 && bs->bytesPerSector != 4096) {
        fprintf(stderr, "fat_init invalid bytesPerSector: %d\n",
                bs->bytesPerSector);
        return false;
    }
    if (bs->sectorsPerCluster == 0 ||
        (bs->sectorsPerCluster & (bs->sectorsPerCluster - 1)) != 0) {
        fprintf(stderr, "fat_init sectorsPerCluster must be a power of 2: %d\n",
                bs->sectorsPerCluster);
        return false;
    }
    if (bs->totalSectors16 == 0) {
        fprintf(stderr,
                "fat_init FAT16/FAT32 (totalSectors32) not supported yet.\n");
        return false;
    }
    if (bs->tableSize16 == 0) {
        fprintf(stderr, "fat_init invalid tableSize16: 0\n");
        return false;
    }

    // region layout derived from the BPB (in sectors)
    uint32_t fatStart = bs->reservedSectorCount;
    uint32_t fatSectors = (uint32_t)bs->tableSize16 * bs->tableCount;
    uint32_t rootDirStart = fatStart + fatSectors;
    uint32_t rootDirSectors =
        ((uint32_t)sizeof(DirectoryEntry) * bs->rootEntryCount +
         bs->bytesPerSector - 1) /
        bs->bytesPerSector;
    uint32_t dataStart = rootDirStart + rootDirSectors;
    // all derived regions must fit and leave at least one data cluster
    uint32_t clusters = dataStart < bs->totalSectors16
                            ? (bs->totalSectors16 - dataStart) /
                                  bs->sectorsPerCluster
                            : 0;
    if (clusters < 1) {
        fprintf(stderr, "fat_init invalid region layout in the BPB.\n");
        return false;
    }

    // the whole image must actually fit in the file
    uint32_t imageSize = (uint32_t)bs->totalSectors16 * bs->bytesPerSector;
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize < (long)imageSize) {
        fprintf(stderr,
                "fat_init file size %ld is smaller than the image size %u.\n",
                fileSize, imageSize);
        return false;
    }

    void* buffer = malloc(imageSize);
    if (buffer == NULL) {
        fprintf(stderr, "fat_init failed to alloc memory.\n");
        return false;
    }

    read = fread(buffer, imageSize, 1, fp);
    if (read != 1) {
        fprintf(stderr, "fat_init failed to read all sectors.\n");
        free(buffer);
        return false;
    }

    // success: publish the globals
    _fat_buffer = buffer;
    _fat_bs = _fat_buffer;
    _fat_start_sector = fatStart;
    _fat_sectors = fatSectors;
    _root_dir_start_sector = rootDirStart;
    _root_dir_sectors = rootDirSectors;
    _data_start_sector = dataStart;
    _data_cluster_count = clusters;
    if (clusters <= 4085)
        _fat_type = FT_FAT12;
    else if (clusters <= 65525)
        _fat_type = FT_FAT16;
    else
        _fat_type = FT_FAT32;

    return true;
}

void fat_uninit(void) {
    if (_fat_buffer != NULL) {
        free(_fat_buffer);
    }
    _fat_buffer = NULL;
    _fat_bs = NULL;
    _fat_type = FT_UNKNOWN;
    _fat_start_sector = 0;
    _fat_sectors = 0;
    _root_dir_start_sector = 0;
    _root_dir_sectors = 0;
    _data_start_sector = 0;
    _data_cluster_count = 0;
}

void increment_color() {
    fat_print_color += 1;
    if (fat_print_color >= CL_GRAY + 1)
        fat_print_color = CL_RED;
}

void fat_print_info() {
    printf("bytesPerSector: %d\n", _fat_bs->bytesPerSector);
    printf("sectorsPerCluster: %d\n", _fat_bs->sectorsPerCluster);
    // 1st FAT table's sector
    printf("reservedSectorCount: %d\n", _fat_bs->reservedSectorCount);
    // count of FAT tables
    printf("tableCount: %d\n", _fat_bs->tableCount);
    printf("rootEntryCount: %d\n", _fat_bs->rootEntryCount);
    printf("root dir sector count: %lu\n", _fat_bs->rootEntryCount *
                                               sizeof(DirectoryEntry) /
                                               _fat_bs->bytesPerSector);
    printf("total_sectors: %d\n", _fat_bs->totalSectors16);
    // count of FAT table sectors
    printf("tableSize16: %d\n", _fat_bs->tableSize16);

    printf("* fat start_sector %d\n", _fat_start_sector);
    printf("* fat sectors %d\n", _fat_sectors);
    printf("* root_dir start_sector %d\n", _root_dir_start_sector);
    printf("* root_dir sectors %d\n", _root_dir_sectors);
    printf("* data start_sector %d\n", _data_start_sector);
    printf("* data cluster count %d\n", _data_cluster_count);
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

void fat_print_header_dump() {
    clcl();

    // 1st line
    int idx = 0;
    const int l[] = {3, 8, 2, 1, 2, -1};
    fat_print_idx_wide((uint8_t*)_fat_bs, &idx, l);
    printf("\n");

    // 2nd line
    idx = 16;
    const int l2[] = {1, 2, 2, 1, 2, 2, 2, 4, -1};
    fat_print_idx_wide((uint8_t*)_fat_bs, &idx, l2);
    printf("\n");

    clcl();
}

void fat_print_legend(const char* legend) {
    cl(fat_print_color);
    printf("- %s\n", legend);
    increment_color();
}

void fat_print_idx_wide(const uint8_t* base, int* idx, const int* lens) {
    int idxStr = *idx;
    fat_print_color = CL_RED;
    for (int i = 0; lens[i] > 0; i++) {
        fat_print_idx((uint8_t*)base, idx, lens[i]);
    }

    fat_print_color = CL_RED;
    for (int i = 0; lens[i] > 0; i++) {
        fat_print_idxstr(base, &idxStr, lens[i]);
    }
}

void fat_print_idx(const uint8_t* base, int* idx, const int len) {
    cl(fat_print_color);
    for (int i = 0; i < len; i++) {
        printf("%02x ", (base)[*idx + i]);
    }
    *idx = *idx + len;
    increment_color();
}

void fat_print_idxstr(const void* base, int* idxStr, const int len) {
    cl(fat_print_color);
    for (int i = 0; i < len; i++) {
        uint8_t u = ((const uint8_t*)base)[*idxStr + i];
        if (40 <= u && u <= 126)
            printf("%c ", u);
        else
            printf("**");
    }
    *idxStr = *idxStr + len;
    increment_color();
}

void fat_print_fat12() {
    // olny print the 1st FAT table
    // FAT12 packs two 12-bit entries into every 3 bytes
    uint32_t entryCount =
        (uint32_t)_fat_bs->tableSize16 * _fat_bs->bytesPerSector * 2 / 3;
    for (uint32_t i = 0; i < entryCount; i++) {
        uint32_t value = fat_get_fat(i);
        printf("%03X ", value);
        if (i % 10 == 9)
            printf("\n");
    }
    printf("\n");
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

void fat_print_directory_entry_dump(DirectoryEntry* entry, void* p) {
    (void)p;  // unused, kept for the callback signature

    // 1st line
    int idx = 0;
    const int l1[] = {11, 1, 1, 1, 2, -1};
    const int l2[] = {2, 2, 2, 2, 2, 2, 4, -1};
    fat_print_idx_wide((uint8_t*)entry, &idx, l1);
    printf("\n");
    // 2nd line
    fat_print_idx_wide((uint8_t*)entry, &idx, l2);
    printf("\n");
    clcl();
}

void fat_print_directory_entry(DirectoryEntry* entry) {
    // Check if entry is unused or deleted
    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5)
        return;

    clcl();

    // Check if entry is a directory
    if (entry->attributes & 0x10) {
        // Directory
        if (entry->name[0] != '.') {
            fat_print_directory_entry_directory(entry, true);
        }
    } else {
        // File entry
        fat_print_directory_entry_file(entry);
    }
}

void fat_print_directory_entry_directory(DirectoryEntry* entry,
                                         bool recursive) {
    // the entry must be a directory
    char directoryName[13];
    memcpy(directoryName, entry->name, 11);
    directoryName[11] = '\0';

    printf("Directory: %s, cluster:%d[0x%08X]\n", directoryName,
           entry->startingClusterNumber,
           fat_get_cluster_addr(entry->startingClusterNumber));
    fat_print_directory_entry_dump(entry, NULL);

    if (recursive) {
        // walk the sub directory's cluster chain
        uint32_t cluster = entry->startingClusterNumber;
        uint32_t entriesPerCluster =
            fat_get_cluster_size() / sizeof(DirectoryEntry);
        // cap the walk at the total data cluster count (+1) against cycles
        for (uint32_t visited = 0; visited <= _data_cluster_count;
             visited++) {
            DirectoryEntry* subEntries =
                (DirectoryEntry*)fat_get_cluster_ptr(cluster);
            if (subEntries == NULL)
                break;  // invalid cluster number: stop the walk
            if (fat_is_broken(cluster))
                break;
            for (uint32_t i = 0; i < entriesPerCluster; i++) {
                DirectoryEntry* sub = &subEntries[i];
                // 0x00: no more entries, 0xE5: deleted
                if (sub->name[0] == 0x00)
                    break;
                if (sub->name[0] == 0xE5)
                    continue;
                if (sub->attributes == 0x0F)
                    continue;  // long file name entry
                fat_print_directory_entry(sub);
            }
            if (fat_is_end_of_cluster(cluster))
                break;
            cluster = fat_get_fat(cluster);
        }
    }
}

void fat_print_directory_entry_file(DirectoryEntry* entry) {
    // the entry must be a file
    char fileName[13];
    memcpy(fileName, entry->name, 11);
    fileName[11] = '\0';

    printf("File: %s, cluster:%d[0x%08X],  size:%d\n", fileName,
           entry->startingClusterNumber,
           fat_get_cluster_addr(entry->startingClusterNumber), entry->fileSize);
    fat_print_directory_entry_dump(entry, NULL);
}

void iterate_rootdir(iterate_dir_callback callback, void* p) {
    FatBS* bs = (FatBS*)fat_get_ptr();

    DirectoryEntry* directoryEntries =
        (DirectoryEntry*)fat_get_root_directory_start_sector_ptr();

    for (uint32_t i = 0; i < (uint32_t)bs->rootEntryCount; i++) {
        DirectoryEntry* entry = &directoryEntries[i];
        // 0x00: no more entries, 0xE5: deleted
        if (entry->name[0] == 0x00)
            break;
        if (entry->name[0] == 0xE5)
            continue;
        if (entry->attributes == 0x0F)
            continue;  // long file name entry
        if (callback != NULL) {
            callback(entry, p);
        }
    }
}

void iterate_dir(uint32_t cluster, iterate_dir_callback callback, void* p) {
    if (cluster == FAT_CLUSTER_ROOT)
        return iterate_rootdir(callback, p);

    uint32_t entriesPerCluster =
        fat_get_cluster_size() / sizeof(DirectoryEntry);

    // walk the cluster chain, capped at the total data cluster count (+1)
    // against cycles in a corrupt FAT
    for (uint32_t visited = 0; visited <= _data_cluster_count; visited++) {
        DirectoryEntry* directoryEntries =
            (DirectoryEntry*)fat_get_cluster_ptr(cluster);
        if (directoryEntries == NULL)
            break;  // invalid cluster number: stop the walk
        if (fat_is_broken(cluster))
            break;

        for (uint32_t i = 0; i < entriesPerCluster; i++) {
            DirectoryEntry* entry = &directoryEntries[i];
            // 0x00: no more entries, 0xE5: deleted
            if (entry->name[0] == 0x00)
                break;
            if (entry->name[0] == 0xE5)
                continue;
            if (entry->attributes == 0x0F)
                continue;  // long file name entry
            if (callback != NULL) {
                callback(entry, p);
            }
        }

        if (fat_is_end_of_cluster(cluster))
            break;
        cluster = fat_get_fat(cluster);
    }
}

char* fat_get_entry_name(DirectoryEntry* entry, char* name, int len) {
    // 8.3 name needs 8 + '.' + 3 + NUL bytes
    if (len < 13) {
        fprintf(stderr, "len must be >=13\n");
        return NULL;
    }
    memcpy(name, (const char*)(entry->name), 8);
    int i;
    for (i = 0; i < 8; i++) {
        if (name[i] == 0x20)
            break;
    }
    if ((entry->attributes & 0x18) == 0) {
        // not Directory or Volume: append the extension
        int extLen = 3;
        while (extLen > 0 && entry->name[8 + extLen - 1] == 0x20)
            extLen--;
        if (extLen > 0) {
            name[i++] = '.';
            memcpy(&name[i], (const char*)&(entry->name[8]), extLen);
            i += extLen;
        }
    }
    name[i] = '\0';
    return name;
}

bool fat_set_entry_name(DirectoryEntry* entry, const char* name) {
    if (name == NULL || name[0] == '\0')
        return false;

    // split into the 8.3 base and extension at the first '.'
    const char* dot = strchr(name, '.');
    size_t baseLen = dot != NULL ? (size_t)(dot - name) : strlen(name);
    const char* ext = dot != NULL ? dot + 1 : NULL;
    size_t extLen = ext != NULL ? strlen(ext) : 0;

    // names that do not fit in 8.3 are not representable
    if (baseLen > 8 || extLen > 3)
        return false;

    memcpy(entry->name, name, baseLen);
    memset(&entry->name[baseLen], 0x20, 8 - baseLen);
    memset(&entry->name[8], 0x20, 3);
    if (extLen > 0) {
        memcpy(&entry->name[8], ext, extLen);
    }

    // to upper
    for (int i = 0; i < 11; i++) {
        if ('a' <= entry->name[i] && entry->name[i] <= 'z') {
            entry->name[i] = entry->name[i] - 'a' + 'A';
        }
    }
    return true;
}

void* fat_get_ptr() { return _fat_buffer; }

enum FAT_TYPE fat_get_type() { return _fat_type; }

void* fat_get_sector_ptr(int sector) {
    void* addr = (uint8_t*)_fat_buffer + sector * _fat_bs->bytesPerSector;
    return addr;
}

void* fat_get_root_directory_start_sector_ptr() {
    return fat_get_sector_ptr(_root_dir_start_sector);
}

uint32_t fat_get_fat(uint32_t cluster) {
    uint8_t* fatp = (uint8_t*)fat_get_sector_ptr(_fat_bs->reservedSectorCount);
    uint32_t value = 0;
    if (cluster % 2 == 0) {
        value = fatp[cluster / 2 * 3];
        value |= ((fatp[cluster / 2 * 3 + 1] & 0x0F) << 8);
    } else {
        value = fatp[cluster / 2 * 3 + 2];
        value = value << 4;
        value |= ((fatp[cluster / 2 * 3 + 1] & 0xF0) >> 4);
    }
    return value;
}

bool fat_is_broken(uint32_t cluster) {
    // assume FAT12, FF7: broken cluster, FF8-FFF: end of cluster chain
    // FAT16, FFF7: broken, FFF8-FFFF: end of cluster chain
    // FAT32, FFFFFFF7: broken, FFFFFFF8-FFFFFFFF: end of cluster chain
    uint32_t fat = fat_get_fat(cluster);
    // assume FAT12
    switch (_fat_type) {
    case FT_FAT12:
        return fat == 0xFF7;
    case FT_FAT16:
        return fat == 0xFFF7;
    case FT_FAT32:
        return fat == 0xFFFFFFF7;
    case FT_UNKNOWN:
        break;
    }
    assert(false);
    return true;
}

bool fat_is_end_of_cluster(uint32_t cluster) {
    uint32_t fat = fat_get_fat(cluster);
    switch (_fat_type) {
    case FT_FAT12:
        return (fat > 0xFF7) ? true : false;
    case FT_FAT16:
        return (fat > 0xFFF7) ? true : false;
    case FT_FAT32:
        return (fat > 0xFFFFFFF7) ? true : false;
    case FT_UNKNOWN:
        break;
    }
    assert(false);
    return true;
}

uint32_t fat_get_cluster_size(void) {
    return (uint32_t)_fat_bs->bytesPerSector * _fat_bs->sectorsPerCluster;
}

uint32_t fat_get_cluster_addr(uint32_t cluster) {
    // valid data clusters are 2 .. _data_cluster_count + 1
    if (cluster < 2 || cluster >= 2 + _data_cluster_count)
        return 0;
    return (_data_start_sector + (cluster - 2) * _fat_bs->sectorsPerCluster) *
           _fat_bs->bytesPerSector;
}

void* fat_get_cluster_ptr(uint32_t cluster) {
    uint32_t addr = fat_get_cluster_addr(cluster);
    if (addr == 0)
        return NULL;
    return (uint8_t*)_fat_buffer + addr;
}

typedef struct {
    DirectoryEntry* entry;
    bool found;
} CallbackFindEntryArg;

void _callback_find_entry(DirectoryEntry* entry, void* p) {
    CallbackFindEntryArg* arg = (CallbackFindEntryArg*)p;
    if (memcmp(arg->entry->name, entry->name, 11) == 0) {
        arg->found = true;
        arg->entry->fileSize = entry->fileSize;
        arg->entry->attributes = entry->attributes;
        arg->entry->creationDate = entry->creationDate;
        arg->entry->creationTime = entry->creationTime;
        arg->entry->creationTimeTenthOfSecond =
            entry->creationTimeTenthOfSecond;
        arg->entry->lastWriteTime = entry->lastWriteTime;
        arg->entry->lastWriteDate = entry->lastWriteDate;
        arg->entry->lastAccessDate = entry->lastAccessDate;
        arg->entry->startingClusterNumber = entry->startingClusterNumber;
    }
}

uint32_t fat_get_cluster_for_entry(uint32_t parent_cluster,
                                   DirectoryEntry* entry) {
    // a blank or all-space name can never match an on-disk entry
    if (entry->name[0] == 0x00 || entry->name[0] == 0x20)
        return FAT_CLUSTER_NOT_FOUND;

    CallbackFindEntryArg arg;
    arg.entry = entry;
    arg.found = false;

    iterate_dir(parent_cluster, _callback_find_entry, &arg);
    if (arg.found) {
        return arg.entry->startingClusterNumber;
    }
    return FAT_CLUSTER_NOT_FOUND;
}
