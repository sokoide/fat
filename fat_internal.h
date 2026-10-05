#ifndef FAT_INTERNAL_H
#define FAT_INTERNAL_H

// Shared internals of the fat library. Included by fat_core.c, fat_dev.c and
// fat_dump.c -- never by main.c/testmain.c; the public contract is fat.h.

#include "fat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// on-disk structs (packed; migrated from the old fat.h) ------------------

// Extended Boot Record (FAT12/16)
typedef struct {
    uint8_t biosDriveNum;
    uint8_t reserved1;
    uint8_t bootSignature;
    uint32_t volumeId;
    uint8_t volumeLabel[11];
    uint8_t fatTypeLabel[8];
} __attribute__((packed)) FatExtBS16;

// // FAT32 extended boot record (kept for phase 2)
// typedef struct {
//     uint32_t table_size_32;
//     uint16_t extended_flags;
//     uint16_t fat_version;
//     uint32_t root_cluster;
//     uint16_t fat_info;
//     uint16_t backup_BS_sector;
//     uint8_t reserved_0[12];
//     uint8_t drive_number;
//     uint8_t reserved_1;
//     uint8_t boot_signature;
//     uint32_t volume_id;
//     uint8_t volume_label[11];
//     uint8_t fat_type_label[8];
// } __attribute__((packed)) FatExtBS32;

// BIOS Parameter Block
typedef struct {
    uint8_t bootJmp[3];
    uint8_t oemName[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint16_t reservedSectorCount;
    uint8_t tableCount;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t mediaType;
    uint16_t tableSize16;
    uint16_t sectorsPerTrack;
    uint16_t headSideCount;
    uint32_t hiddenSectorCount;
    uint32_t totalSectors32;
    // cast to FatExtBS16/32 once the driver knows the FAT type
    uint8_t extended_section[54];
} __attribute__((packed)) FatBS;

// directory entry (32 bytes on disk)
typedef struct {
    uint8_t name[11];
    uint8_t attributes;
    uint8_t reserved[1];
    uint8_t creationTimeTenthOfSecond;
    uint16_t creationTime;
    uint16_t creationDate;
    uint16_t lastAccessDate;
    uint16_t firstClusterHigh;  // ignoreInFAT12; FAT32 high 16 bits
    uint16_t lastWriteTime;
    uint16_t lastWriteDate;
    uint16_t firstClusterLow;
    uint32_t fileSize;
} __attribute__((packed)) DirectoryEntry;

// attribute bits
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LONG_NAME 0x0F

// context ----------------------------------------------------------------

struct fat_ctx {
    uint8_t* image;      // whole image copy (owned)
    size_t image_size;
    enum FAT_TYPE type;
    fat_geometry_t geo;  // validated, host-endian
};

// internal region/FAT accessors (fat_core.c) ------------------------------

// Raw 12-bit FAT entry of `cluster`; no bounds check on top of geo
// (fat_get_fat_entry validates). Caller ensures a valid context.
uint32_t fat_raw_fat12(const fat_ctx_t* ctx, uint32_t cluster);

// Pointer into the image at byte `offset`, or NULL when out of range.
const uint8_t* fat_region_ptr(const fat_ctx_t* ctx, size_t offset);

// Pointer to the first FAT table, or NULL when out of range.
const uint8_t* fat_fat_ptr(const fat_ctx_t* ctx);

// Core initializer shared with fat_dev.c: validate the BPB, copy `size`
// bytes of `image` into a fresh context, bind `*out`.
fat_result_t fat_ctx_init_mem(fat_ctx_t** out, const uint8_t* image,
                              size_t size);

#endif
