#include "color.h"
#include "fat.h"
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// function declaration
void check_null(void* p);
void increment_color(int* color);
void print_legend(int* color, char* legend);
void dump_idx(uint8_t* b, int* idx, int len, int* color);
void dump_header(uint8_t* b);
void dump_entry(DirectoryEntry* e);
void dump_sector(uint8_t* b, int first_sector, int len, int sector_size);
void dump(uint8_t* p, int offset, int len);

// functions
void check_null(void* p) {
    if (p == NULL) {
        fprintf(stderr, "p is NULL.\n");
        exit(1);
    }
}

void increment_color(int* color) {
    *color += 1;
    if (*color >= CL_GRAY + 1)
        *color = CL_RED;
}

void print_legend(int* color, char* legend) {
    cl(*color);
    printf("- %s\n", legend);
    increment_color(color);
}

void dump_idx(uint8_t* b, int* idx, int len, int* color) {
    cl(*color);
    /* for (int i = *idx; i < *idx + len; i++) { */
    /*     printf("%02x ", b[i]); */
    /* } */
    for (int i = 0; i < len; i++) {
        printf("%02x ", b[*idx + i]);
    }
    *idx = *idx + len;
    increment_color(color);
}

void dump_header(uint8_t* b) {
    clcl();
    printf("*** BIOS parameter block ***\n");

    int color = CL_RED;
    print_legend(&color, "relative jump (eb3c) + nop (90)");
    print_legend(&color, "OEM Name");
    print_legend(&color, "bytes per sector");
    print_legend(&color, "sectors per cluster");
    print_legend(&color, "FAT table's 1st sector (reserved sectors)");
    print_legend(&color, "FAT table count");
    print_legend(&color, "Max entries in root table");
    print_legend(&color, "Total sector count");
    print_legend(&color, "Media type");
    print_legend(&color, "sectors per FAT table");
    print_legend(&color, "sectors per track");
    print_legend(&color, "head count");
    print_legend(&color, "hidden sectors");
    printf("\n");

    // 1st line
    int idx = 0;
    color = CL_RED;
    dump_idx(b, &idx, 3, &color);
    dump_idx(b, &idx, 8, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 1, &color);
    dump_idx(b, &idx, 2, &color);
    printf("\n");

    // 2nd line
    idx = 16;
    dump_idx(b, &idx, 1, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 1, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 4, &color);
    clcl();
    printf("\n\n");
}

void dump_entry(DirectoryEntry* e) {
    uint8_t* b = (uint8_t*)e;
    printf("*** entry ***\n");
    int color = CL_RED;
    print_legend(&color, "Name");
    print_legend(&color, "Attributes");
    print_legend(&color, "(reserved)");
    print_legend(&color, "Creation time (millisec)");
    print_legend(&color, "Creation time");
    print_legend(&color, "Creation date");
    print_legend(&color, "Last access date");
    print_legend(&color, "(ignored in FAT12)");
    print_legend(&color, "Last write time");
    print_legend(&color, "Last write date");
    print_legend(&color, "Starting cluster");
    print_legend(&color, "File size");

    int idx = 0;
    color = CL_RED;
    dump_idx(b, &idx, 11, &color);
    dump_idx(b, &idx, 1, &color);
    dump_idx(b, &idx, 1, &color);
    dump_idx(b, &idx, 1, &color);
    dump_idx(b, &idx, 2, &color);
    printf("\n");
    // 2nd line
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 2, &color);
    dump_idx(b, &idx, 4, &color);

    clcl();
    printf("\n\n");
}

void dump_sector(uint8_t* b, int first_sector, int len, int sector_size) {
    for (int s = first_sector; s < first_sector + len; s++) {
        dump(b, s * sector_size, sector_size);
    }
}

void dump(uint8_t* p, int offset, int len) {
    uint8_t* buffer = p + offset;
    for (int i = 0; i < len; i++) {
        uint8_t b = buffer[i];
        if (i % 16 == 0) {
            printf("%08x: ", i + offset);
        }

        printf("%02x ", b);
        if (i % 16 == 15) {
            printf("\n");
        }
    }
}

int main() {
    char fat_path[128];
    /* sprintf(fat_path, "%s/%s", getenv("HOME"), */
    /*         "koidos.fat"); */
    strcpy(fat_path, "demof12.fat");
    FILE* fp = fopen(fat_path, "rb");
    if (fp == NULL) {
        printf("failed to open the fat image '%s'.\n", fat_path);
        return 1;
    }


    uint8_t* buffer = malloc(1024 * 1024 + 1); // 1MB
    check_null(buffer);
    fread(buffer, 1024 * 1024, 1, fp);
    dump_header(buffer);

    // ref: https://free.pjc.co.jp/fat/mem/fatm122.html
    // FAT12
    FatBS* p = (FatBS*)buffer;
    printf("bytesPerSector: %d\n", p->bytesPerSector);
    printf("sectorsPerCluster: %d\n", p->sectorsPerCluster);
    // 1st FAT table's sector
    printf("reservedSectorCount: %d\n", p->reservedSectorCount);
    // count of FAT tables
    printf("tableCount: %d\n", p->tableCount);
    printf("rootEntryCount: %d\n", p->rootEntryCount);
    int root_dir_sector_count = p->rootEntryCount * 32 / p->bytesPerSector;
    printf("total_sectors: %d\n", p->totalSectors16);
    // cout of FAT table sectors
    printf("tableSize16: %d\n", p->tableSize16);


    int fat_start_sector = p->reservedSectorCount;
    int fat_sectors = p->tableSize16 * p->tableCount;
    int root_dir_start_sector = fat_start_sector + fat_sectors;
    int root_dir_sectors = (sizeof(DirectoryEntry) * p->rootEntryCount + p->bytesPerSector -1)/ p->bytesPerSector;
    int root_dir_start_addr = root_dir_start_sector * p->bytesPerSector;
    int data_start_sector = root_dir_start_sector + root_dir_sectors;
    int data_sectors = p->totalSectors16 - data_start_sector;
    int data_start_addr = data_start_sector * p->bytesPerSector;
    printf("* fat start_sector %d\n", fat_start_sector);
    printf("* fat sectors %d\n", fat_sectors);
    printf("* root_dir start_sector %d\n", root_dir_start_sector);
    printf("* root_dir sectors %d\n", root_dir_sectors);
    printf("* data start_sector %d\n", data_start_sector);
    printf("* data start_addr 0x%X\n", data_start_addr);

    dump(buffer, data_start_addr, 32);

    // FAT tables
    printf("*** FAT tables ***\n");
    for (int i = 0; i < p->tableCount; i++) {
        printf("* FAT table %d\n", i);
        dump_sector(buffer, p->reservedSectorCount + i * p->tableSize16,
                    p->tableSize16, p->bytesPerSector);
    }

    // Seek to the root dir
    uint8_t* p2 = ((uint8_t*)buffer) + root_dir_start_addr;

    // Read the root directory entries
    DirectoryEntry* directoryEntries = (DirectoryEntry*)p2;

    // Traverse the root directory entries
    for (int i = 0; i < p->rootEntryCount; i++) {
        DirectoryEntry* entry = &directoryEntries[i];

        // Check if entry is unused or deleted
        if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
            continue;
        }

        // Check if entry is a directory
        dump_entry(entry);
        if (entry->attributes & 0x10) {
            // Directory entry
            char directoryName[13];
            memcpy(directoryName, entry->name, 11);
            directoryName[11] = '\0';

            // Omit '.' and '..' directories
            if (strcmp(directoryName, ".") == 0 ||
                strcmp(directoryName, "..") == 0) {
                continue;
            }

            printf("Directory: %s, cluster:%d\n", directoryName,
                   entry->startingClusterNumber);
        } else {
            // File entry
            char fileName[13];
            memcpy(fileName, entry->name, 11);
            fileName[11] = '\0';
            int cluster_addr = (data_start_sector +( entry->startingClusterNumber - 2) * p->sectorsPerCluster) * p->bytesPerSector;

            printf("File: %s, cluster:%d[0x%08X],  size:%d\n", fileName, entry->startingClusterNumber, cluster_addr, entry->fileSize);
        }
    }

CLEANUP:
    free(buffer);
    fclose(fp);

    return 0;
}
