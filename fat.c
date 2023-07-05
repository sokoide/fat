#include "fat.h"
#include "color.h"
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// globals
void* _fat_buffer;
FatBS* _fat_bs;

// functions
bool fat_init(FILE* fp /* in */) {
    uint8_t b[512];
    _fat_bs = (FatBS*)&b;
    fseek(fp, 0, SEEK_SET);

    size_t read = fread(b, sizeof(b) / sizeof(b[0]), 1, fp);
    if (read != 1) {
        fprintf(stderr, "fat_init failed to read the boot sector.\n");
        return false;
    }

    _fat_buffer = malloc(_fat_bs->totalSectors16 * _fat_bs->bytesPerSector);
    if (_fat_buffer == NULL) {
        fprintf(stderr, "fat_init failed to alloc memory.\n");
        return false;
    }

    fseek(fp, 0, SEEK_SET);
    read = fread(_fat_buffer, _fat_bs->totalSectors16 * _fat_bs->bytesPerSector,
                 1, fp);
    if (read != 1) {
        fprintf(stderr, "fat_init failed to read all sectors.\n");
        return false;
    }
    return true;
}

void fat_unint() {
    if (_fat_buffer != NULL) {
        free(_fat_buffer);
    }
    _fat_buffer = NULL;
}

void* fat_get_sector(int sector /* in */) {
    void* addr = _fat_buffer + sector * _fat_bs->bytesPerSector;
    return addr;
}

bool fat_get_directory_entry_start(FILE* fp /* in */, uint8_t* buffer /* out */,
                                   int buffer_size /* in */) {
    int fat_start_sector = _fat_bs->reservedSectorCount;
    int fat_sectors = _fat_bs->tableSize16 * _fat_bs->tableCount;
    int root_dir_start_sector = fat_start_sector + fat_sectors;
    /* int root_dir_sectors = */
    /*     (sizeof(DirectoryEntry) * _fat_bs->rootEntryCount +
     * _fat_bs->bytesPerSector
     * - 1) / */
    /*     _fat_bs->bytesPerSector; */
    /* int root_dir_start_addr = root_dir_start_sector *
     * _fat_bs->bytesPerSector;
     */
    /* int data_start_sector = root_dir_start_sector + root_dir_sectors; */
    /* int data_sectors = _fat_bs->totalSectors16 - data_start_sector; */
    /* int data_start_addr = data_start_sector * _fat_bs->bytesPerSector; */

    return fat_get_sector(root_dir_start_sector);
}

uint32_t fat_get_fat(int cluster) {
    // assume FAT12
    uint8_t* fatp = (uint8_t*)fat_get_sector(_fat_bs->reservedSectorCount);
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
