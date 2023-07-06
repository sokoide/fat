#ifndef _FAT_H_
#define _FAT_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// enum
enum FAT_TYPE {
    FT_UNKNOWN = 0,
    FT_FAT12 = 1,
    FT_FAT16 = 2,
    FT_FAT32 = 3,
};

// globals

// structs
//
// // FAT32
// typedef struct {
//     // extended fat32 stuff
//     unsigned int table_size_32;
//     unsigned short extended_flags;
//     unsigned short fat_version;
//     unsigned int root_cluster;
//     unsigned short fat_info;
//     unsigned short backup_BS_sector;
//     unsigned char reserved_0[12];
//     unsigned char drive_number;
//     unsigned char reserved_1;
//     unsigned char boot_signature;
//     unsigned int volume_id;
//     unsigned char volume_label[11];
//     unsigned char fat_type_label[8];

// } __attribute__((packed)) FatExtBS32;

// Extended Boot Record
typedef struct {
    // extended fat12 and fat16 stuff
    unsigned char biosDriveNum;
    unsigned char reserved1;
    unsigned char bootSignature;
    unsigned int volumeId;
    unsigned char volumeLabel[11];
    unsigned char fatTypeLabel[8];

} __attribute__((packed)) FatExtBS16;

// BIOS Parameter Block
typedef struct {
    unsigned char bootJmp[3];
    unsigned char oemName[8];
    unsigned short bytesPerSector;
    unsigned char sectorsPerCluster;
    unsigned short reservedSectorCount;
    unsigned char tableCount;
    unsigned short rootEntryCount;
    unsigned short totalSectors16;
    unsigned char mediaType;
    unsigned short tableSize16;
    unsigned short sectorsPerTrack;
    unsigned short headSideCount;
    unsigned int hiddenSectorCount;
    unsigned int totalSectors32;

    // this will be cast to it's specific type once the driver actually knows
    // what type of FAT this is.
    unsigned char extended_section[54];

} __attribute__((packed)) FatBS;

// ref: https://wiki.osdev.org/FAT#Directories_on_FAT12.2F16.2F32
typedef struct DirectoryEntry {
    unsigned char name[11];
    unsigned char attributes;
    unsigned char reserved[1];
    unsigned char creationTimeTenthOfSecond;
    unsigned short creationTime;
    unsigned short creationDate;
    unsigned short lastAccessDate;
    unsigned short ignoreInFAT12;
    unsigned short lastWriteTime;
    unsigned short lastWriteDate;
    unsigned short startingClusterNumber;
    unsigned int fileSize;
} __attribute__((packed)) DirectoryEntry;

// functions
bool fat_init(FILE* fp);
void fat_unint();

void increment_color();
void fat_print_info();
void fat_print_header_legend();
void fat_print_header_dump();
void fat_print_legend(char* legend);
void fat_print_idx_wide(uint8_t* base, int* idx, const int* lens);
void fat_print_idx(uint8_t* base, int* idx, int len);
void fat_print_idxstr(void* base, int* idxStr, int len);
void fat_print_fat12();
void fat_print_directory_entry_header_legend();
void fat_print_directory_entry_dump(DirectoryEntry* entry);
void fat_print_directory_entry(DirectoryEntry* entry);
void fat_print_directory_entry_directory(DirectoryEntry* entry, bool recursive);
void fat_print_directory_entry_file(DirectoryEntry* entry);
void iterate_dir(uint32_t cluster);
void iterate_rootdir();

void* fat_get_ptr();
enum FAT_TYPE fat_get_type();
void* fat_get_sector_ptr(int sector);
void* fat_get_root_directory_start_sector_ptr();
uint32_t fat_get_fat(uint32_t cluster);
uint32_t fat_get_cluster_addr(int cluster);
void* fat_get_cluster_ptr(int cluster);

#endif
