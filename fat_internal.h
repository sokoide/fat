#ifndef FAT_INTERNAL_H
#define FAT_INTERNAL_H

// Shared internals of the fat library. Included by fat_core.c, fat_dev.c and
// fat_dump.c -- never by main.c/testmain.c; the public contract is fat.h.

#include "fat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// on-disk structs (packed; migrated from the old fat.h) ------------------

// Extended Boot Record common tail: at BPB offset 36 for FAT12/16, and at
// BPB offset 64 (= extended_section[28]) for FAT32, right after FatExtBS32.
typedef struct {
    uint8_t biosDriveNum;
    uint8_t reserved1;
    uint8_t bootSignature;
    uint32_t volumeId;
    uint8_t volumeLabel[11];
    uint8_t fatTypeLabel[8];
} __attribute__((packed)) FatExtBS16;

// FAT32-specific Extended Boot Record: the 28 type bytes at BPB offset 36.
// The FatExtBS16 common tail follows at BPB offset 64.
typedef struct {
    uint32_t fatsz32;   // sectors per FAT table when tableSize16 == 0
    uint16_t extFlags;  // bit7: no mirroring, bits3..0: active FAT number
    uint16_t fsVer;
    uint32_t rootClus;  // first cluster of the root directory chain
    uint16_t fsInfo;    // FSInfo sector number (0 = none)
    uint16_t bkBootSec; // backup boot sector number
    uint8_t reserved[12];
} __attribute__((packed)) FatExtBS32;

// compile-time size checks (C99 has no _Static_assert)
typedef char fat_extbs16_size_check[(sizeof(FatExtBS16) == 26) * 2 - 1];
typedef char fat_extbs32_size_check[(sizeof(FatExtBS32) == 28) * 2 - 1];

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
    // cast to FatExtBS16 at [0] for FAT12/16; for FAT32, FatExtBS32 occupies
    // [0..27] and the FatExtBS16 common tail starts at [28]
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

// direct-mapped sector cache: 16 slots of one sector each. The slot index
// is `sector % 16`; collisions evict (flushing a dirty victim to the
// backend first -- an early flush, permitted before fat_sync).
#define FAT_CACHE_SLOTS 16

typedef struct {
    bool valid;
    bool dirty;
    uint64_t sector; // sector number this slot currently holds
    uint8_t* data;   // ctx->cache_buf + slot_index * bytes_per_sector
} fat_cache_slot_t;

struct fat_ctx {
    fat_io_t* io;       // owned backend (bound on a successful open)
    size_t image_size;  // io->size() snapshot taken at open
    enum FAT_TYPE type;
    fat_geometry_t geo; // validated, host-endian
    uint16_t fsinfo_sector; // FAT32 BPB FSInfo sector; 0 = none (FAT12/16)
    uint16_t ext_flags;     // FAT32 BPB_ExtFlags snapshot; 0 otherwise
    fat_cache_slot_t cache[FAT_CACHE_SLOTS];
    uint8_t* cache_buf; // FAT_CACHE_SLOTS * geo.bytes_per_sector bytes
};

// internal region/FAT accessors (fat_core.c) ------------------------------

// Raw FAT[cluster] value, unpacked per the image type (12/16/32-bit wide;
// FAT32 values masked with 0x0FFFFFFF), read through the sector cache.
// Failure modes are distinct:
//   FAT_OK              -> *out holds the entry
//   FAT_ERR_IO          -> backend read failure
//   FAT_ERR_INVALID_BPB -> the FAT region does not cover the index
// Caller ensures a valid context and index range.
fat_result_t fat_raw_fat_entry(fat_ctx_t* ctx, uint32_t cluster, uint32_t* out);

// Cache-mediated transfers over the backend: any span (a FAT12 entry may
// straddle a sector boundary), bounds-checked against [0, image_size);
// zero-length transfers are FAT_OK and touch nothing.
fat_result_t fat_io_read(fat_ctx_t* ctx, uint64_t offset, void* buf, size_t len);
fat_result_t fat_io_write(fat_ctx_t* ctx, uint64_t offset, const void* buf,
                          size_t len);

// Core initializer: validate the BPB through `io` (exactly one 512-byte
// boot-sector read; a backend that cannot serve it fails with FAT_ERR_IO)
// and bind `*out`. On success the context owns `io` (fat_close flushes and
// closes it); on failure `io` is left untouched -- the caller keeps it.
fat_result_t fat_ctx_init(fat_io_t* io, fat_ctx_t** out);

#endif
