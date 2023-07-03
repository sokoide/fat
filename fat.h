#ifndef _FAT_H_
#define _FAT_H_

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

// FAT32
//
// * FAT part
// http://park12.wakwak.com/~eslab/pcmemo/fdfat/fdfat4.html#4.1
//
// cluster 0: media type: 0xFF0=3.5 inch FD, 0xFF8=HDD
// cluster 1: always 0xFFF
// cluster 2-: data area
//
// * Data area part
//

typedef struct fat_extBS_32 {
    // extended fat32 stuff
    unsigned int table_size_32;
    unsigned short extended_flags;
    unsigned short fat_version;
    unsigned int root_cluster;
    unsigned short fat_info;
    unsigned short backup_BS_sector;
    unsigned char reserved_0[12];
    unsigned char drive_number;
    unsigned char reserved_1;
    unsigned char boot_signature;
    unsigned int volume_id;
    unsigned char volume_label[11];
    unsigned char fat_type_label[8];

} __attribute__((packed)) fat_extBS_32_t;

// Extended Boot Record
typedef struct fat_extBS_16 {
    // extended fat12 and fat16 stuff
    unsigned char bios_drive_num;
    unsigned char reserved1;
    unsigned char boot_signature;
    unsigned int volume_id;
    unsigned char volume_label[11];
    unsigned char fat_type_label[8];

} __attribute__((packed)) fat_extBS_16_t;

// BIOS Parameter Block
typedef struct fat_BS {
    // 0
    unsigned char bootjmp[3];
    unsigned char oem_name[8];
    unsigned short bytes_per_sector;
    unsigned char sectors_per_cluster;
    // 14 .. if this is 1, the 1st FAT table is in sector 1
    unsigned short reserved_sector_count;
    // 16
    unsigned char table_count;
    // 17
    unsigned short root_entry_count;
    // 19 .. sectors per FAT
    unsigned short total_sectors_16;
    // 21
    unsigned char media_type;
    // 22
    unsigned short table_size_16;
    unsigned short sectors_per_track;
    unsigned short head_side_count;
    unsigned int hidden_sector_count;
    unsigned int total_sectors_32;

    // this will be cast to it's specific type once the driver actually knows
    // what type of FAT this is.
    unsigned char extended_section[54];

} __attribute__((packed)) FatBS;

// ref: https://wiki.osdev.org/FAT#Directories_on_FAT12.2F16.2F32
typedef struct DirectoryEntry {
    // 0
    unsigned char name[11];
    // 11
    unsigned char attributes;
    // 12
    unsigned char reserved[1];
    // 13
    unsigned char creationTimeTenthOfSecond;
    // 14
    unsigned short creationTime;
    unsigned short creationDate;
    // 18
    unsigned short lastAccessDate;
    // 20
    unsigned short ignoreInFAT12;
    // 22
    unsigned short lastWriteTime;
    // 24
    unsigned short lastWriteDate;
    // 26
    unsigned short startingClusterNumber;
    // 28
    unsigned int fileSize;
} __attribute__((packed)) DirectoryEntry;

#endif
