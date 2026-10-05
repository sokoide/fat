#ifndef FAT_H
#define FAT_H

#include <stddef.h>
#include <stdint.h>

// cluster sentinels
#define FAT_CLUSTER_ROOT 0u                // root directory (FAT12/16 fixed region)
#define FAT_CLUSTER_NOT_FOUND 0xFFFFFFFFu  // no FAT entry / out of range

#define FAT_NAME_MAX 256 // 8.3 today, LFN-ready size

// FAT flavor of an opened image
enum FAT_TYPE {
    FT_UNKNOWN = 0,
    FT_FAT12 = 1,
    FT_FAT16 = 2, // supported
    FT_FAT32 = 3, // supported (root directory is a cluster chain)
};

// every public function returns this; FAT_OK == 0 on success
typedef enum {
    FAT_OK = 0,
    FAT_ERR_IO,              // file read failure
    FAT_ERR_NOMEM,
    FAT_ERR_INVALID_BPB,     // malformed boot sector / geometry
    FAT_ERR_UNSUPPORTED,     // e.g. FAT16/FAT32 images
    FAT_ERR_NAME_TOO_LONG,
    FAT_ERR_BUFFER_TOO_SMALL,
    FAT_ERR_INVALID_ARG,
    FAT_ERR_NOT_FOUND,       // final path component not found
    FAT_ERR_PATH_NOT_FOUND,  // intermediate directory missing or not a dir
    FAT_ERR_BAD_CLUSTER,     // broken/looping cluster chain
} fat_result_t;

const char* fat_strerror(fat_result_t r);

// parsed directory entry (name rendered from 8.3; LFN-ready size)
typedef struct {
    char name[FAT_NAME_MAX];
    uint8_t attributes;
    uint8_t creation_time_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t last_write_time;
    uint16_t last_write_date;
    uint32_t first_cluster;
    uint32_t file_size;
} fat_dirent_t;

// iteration callback: parsed entry + the 32 raw on-disk bytes (dump views).
// raw32 is only valid during the callback.
typedef void (*fat_iter_cb)(const fat_dirent_t* entry, const uint8_t* raw32,
                            void* user_data);

// opaque context; internal definition lives in fat_internal.h
typedef struct fat_ctx fat_ctx_t;

// validated, host-endian geometry of an opened image
typedef struct {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t fat_sectors;   // sectors per FAT table (tableSize16)
    uint16_t root_entries;
    uint32_t total_sectors; // totalSectors16, or totalSectors32 when that is 0
    uint32_t fat_start_sector;
    uint32_t root_dir_sector;   // FAT12/16 fixed root region (0 for FAT32)
    uint32_t root_dir_sectors;  // FAT12/16 only (0 for FAT32)
    uint32_t root_cluster;      // FAT32: first cluster of the root chain; 0 for FAT12/16
    uint32_t data_start_sector;
    uint32_t cluster_count; // data clusters; valid data clusters are 2..cluster_count+1
} fat_geometry_t;

// lifecycle -----------------------------------------------------------

// Load the whole image from `path`, validate the BPB, bind `*out`.
// FAT12, FAT16 and FAT32 images are supported.
fat_result_t fat_open(const char* path, fat_ctx_t** out);

// Same, from a memory image. The buffer is copied; the caller keeps
// ownership of `image`. Enables in-process synthetic/mutated images.
fat_result_t fat_open_mem(const uint8_t* image, size_t size, fat_ctx_t** out);

// Release everything. Safe on NULL. The context is unusable afterwards.
void fat_close(fat_ctx_t* ctx);

// introspection -------------------------------------------------------

enum FAT_TYPE fat_get_type(const fat_ctx_t* ctx);          // NULL -> FT_UNKNOWN
const fat_geometry_t* fat_geometry(const fat_ctx_t* ctx);  // NULL -> ctx NULL
uint32_t fat_cluster_size(const fat_ctx_t* ctx);           // bytes per data cluster

// Raw FAT[cluster] value, unpacked (12/16/32-bit wide depending on the
// image type; FAT32 values are masked with 0x0FFFFFFF). Indices
// 0..cluster_count+1 are readable (0/1 hold the media/reserved entries);
// larger is an error.
fat_result_t fat_get_fat_entry(const fat_ctx_t* ctx, uint32_t cluster,
                               uint32_t* out);

// directory iteration --------------------------------------------------

// Iterate `dir_cluster` (FAT_CLUSTER_ROOT for the root directory).
// Skips deleted (0xE5) and LFN (attr 0x0F) entries; stops at the first
// never-used (0x00) entry; guards against broken/looping chains.
fat_result_t fat_iter_dir(fat_ctx_t* ctx, uint32_t dir_cluster,
                          fat_iter_cb cb, void* user_data);

// path lookup -----------------------------------------------------------

// Resolve `path` relative to `start_cluster` (FAT_CLUSTER_ROOT = absolute).
// '/'-separated; empty components ignored; "." stays in the current
// directory, ".." resolves via the directory's own dot entries (".." of a
// root-level directory yields FAT_CLUSTER_ROOT; ".." at the root itself
// fails with FAT_ERR_PATH_NOT_FOUND).
// Intermediate components must have ATTR_DIRECTORY (0x10); the final
// component may be a file or a directory.
//   FAT_OK               -> *out filled
//   FAT_ERR_NOT_FOUND    -> final component missing
//   FAT_ERR_PATH_NOT_FOUND -> intermediate missing / not a directory
fat_result_t fat_lookup(fat_ctx_t* ctx, uint32_t start_cluster,
                        const char* path, fat_dirent_t* out);

// 8.3 name conversion ----------------------------------------------------

// Render an 11-byte on-disk name. Files get "NAME.EXT" with trailing
// spaces trimmed; directories and volume labels get no extension dot.
fat_result_t fat_name_from_83(const uint8_t name11[11], uint8_t attributes,
                              char* out, size_t out_len);

// Inverse: validate, uppercase, space-pad into name11. Rejects NULL/empty
// names, base > 8, extension > 3, and "." / ".." (fat_lookup handles those
// itself). Characters after the first '.' count toward the extension.
fat_result_t fat_name_to_83(const char* name, uint8_t name11[11]);

// FSInfo (FAT32 only) ------------------------------------------------------

typedef struct {
    uint32_t free_cluster_count; // 0xFFFFFFFF = unknown/stale
    uint32_t next_free_cluster;  // 0xFFFFFFFF = unknown
} fat_fsinfo_t;

// Parse the FSInfo sector. FAT_ERR_UNSUPPORTED for FAT12/16. Values are
// 0xFFFFFFFF when the sector is absent or its signatures are invalid.
fat_result_t fat_fsinfo(const fat_ctx_t* ctx, fat_fsinfo_t* out);

// file read ---------------------------------------------------------------

// Read the whole file into a malloc'd buffer; caller frees with free().
// An empty file returns FAT_OK with *out == NULL, *out_size == 0.
//   FAT_ERR_INVALID_ARG  -> entry is not a regular file (dir/volume/label)
//   FAT_ERR_BAD_CLUSTER  -> chain ends before file_size or loops
fat_result_t fat_read_file(fat_ctx_t* ctx, const fat_dirent_t* file,
                           uint8_t** out, size_t* out_size);

#endif
