#ifndef FAT_H
#define FAT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h> // struct tm for the timestamp helpers

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
    FAT_ERR_UNSUPPORTED,     // operation not valid for this image type
    FAT_ERR_NAME_TOO_LONG,
    FAT_ERR_BUFFER_TOO_SMALL,
    FAT_ERR_INVALID_ARG,
    FAT_ERR_NOT_FOUND,       // final path component not found
    FAT_ERR_PATH_NOT_FOUND,  // intermediate directory missing or not a dir
    FAT_ERR_BAD_CLUSTER,     // broken/looping cluster chain
    FAT_ERR_END_OF_DIR,      // directory cursor exhausted (fat_dir_next)
    FAT_ERR_DISK_FULL,       // no free cluster left (fat_alloc_cluster)
    FAT_ERR_DIR_FULL,        // FAT12/16 root directory is full
    FAT_ERR_EXISTS,          // a live entry with this name already exists
    FAT_ERR_DIR_NOT_EMPTY,   // fat_rmdir target holds live entries
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

// Open the image at `path` through a file backend (fat_io_file),
// validate the BPB, bind `*out`. FAT12, FAT16 and FAT32 images are
// supported. The image is not loaded whole: accesses go through the
// sector cache, and writes reach the file no later than
// fat_sync()/fat_close().
fat_result_t fat_open(const char* path, fat_ctx_t** out);

// Same, through a RAM backend (fat_io_mem) over a private copy of the
// buffer; the caller keeps ownership of `image`. Enables in-process
// synthetic/mutated images.
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
// directory, ".." resolves via the directory's own dot entries.
// "." and ".." at the root itself succeed with a synthetic root dirent
// (name "."/"..", ATTR_DIRECTORY, first_cluster FAT_CLUSTER_ROOT, size 0),
// matching DOS semantics -- the root is its own parent.
// Intermediate components must have ATTR_DIRECTORY (0x10); the final
// component may be a file or a directory. Volume-label entries
// (ATTR_VOLUME_ID) never match, like DOS open().
//   FAT_OK               -> *out filled
//   FAT_ERR_NOT_FOUND    -> final component missing (or is a volume label)
//   FAT_ERR_PATH_NOT_FOUND -> intermediate missing / not a directory
fat_result_t fat_lookup(fat_ctx_t* ctx, uint32_t start_cluster,
                        const char* path, fat_dirent_t* out);

// 8.3 name conversion ----------------------------------------------------

// Render an 11-byte on-disk name. Files get "NAME.EXT" with trailing
// spaces trimmed; directories and volume labels get no extension dot.
// An on-disk first byte of 0x05 renders as 0xE5 (the Japanese-name escape:
// a name whose true first byte is 0xE5 is stored as 0x05 so it is not
// mistaken for a deleted entry).
fat_result_t fat_name_from_83(const uint8_t name11[11], uint8_t attributes,
                              char* out, size_t out_len);

// Inverse: validate, uppercase, space-pad into name11. Rejects NULL/empty
// names, base > 8, extension > 3, and "." / ".." (fat_lookup handles those
// itself). Characters after the first '.' count toward the extension.
fat_result_t fat_name_to_83(const char* name, uint8_t name11[11]);

// streaming file access ----------------------------------------------------

// Opaque cursor over one file's cluster chain; decouples access from
// whole-file allocation (fat_read_file) and survives interleaved seeks.
// Read cursors (fat_file_open) are pure views; write cursors
// (fat_file_open_write) can also truncate and extend the file in place.
typedef struct fat_file fat_file_t;

// Bind a cursor to a regular file. The dirent is validated and copied, so
// `file` may be a stack object. Same rules as fat_read_file:
//   FAT_ERR_INVALID_ARG -> entry is not a regular file (dir/volume/label)
fat_result_t fat_file_open(fat_ctx_t* ctx, const fat_dirent_t* file,
                           fat_file_t** out);

// Current byte position / total size.
uint64_t fat_file_tell(const fat_file_t* f); // 0 for NULL
uint64_t fat_file_size(const fat_file_t* f); // 0 for NULL

// Absolute seek within [0, file_size] (position == size is EOF and legal).
// Beyond the end is FAT_ERR_INVALID_ARG. Seek is O(chain length): the
// cluster chain is re-walked from the start.
fat_result_t fat_file_seek(fat_file_t* f, uint64_t offset);

// Read up to `len` bytes at the cursor into `buf`; *read receives the
// byte count (0 at EOF). A short read only happens at EOF -- otherwise
// the full `len` is delivered. Broken/looping chains interrupt the read
// with FAT_ERR_BAD_CLUSTER (same guards as fat_read_file); *read is only
// meaningful after FAT_OK.
fat_result_t fat_file_read(fat_file_t* f, uint8_t* buf, size_t len,
                           size_t* read);

// Open the existing regular file `name` in `dir_cluster` (FAT_CLUSTER_ROOT
// = the root directory) for reading AND writing. Unlike fat_file_open the
// cursor is bound to the file's directory slot, so fat_file_truncate /
// fat_file_write can update the entry's first-cluster and file-size fields
// as they go. Same validation as fat_file_open (a regular file is
// required); creation stays with fat_write_file:
//   FAT_ERR_NOT_FOUND    -> no live entry with this name
//   FAT_ERR_INVALID_ARG  -> entry is not a regular file, or ATTR_READ_ONLY
fat_result_t fat_file_open_write(fat_ctx_t* ctx, uint32_t dir_cluster,
                                 const char* name, fat_file_t** out);

// Resize the file to exactly `size` bytes. Growing zero-fills the new
// bytes (tail of the last cluster plus newly allocated clusters);
// shrinking frees the clusters that fall entirely past the end -- the
// kept partial tail keeps its leftover bytes (file_size bounds readers).
// Truncating to 0 releases the whole chain and resets the entry's
// first_cluster to 0, like a freshly created empty file. The cursor
// position is clamped to the new size. Consistency order: the dirent
// size is updated before clusters are freed (shrink) and after the
// extension is in place (grow), so no intermediate state dangles.
// Timestamps are left untouched (determinism; manage them yourself).
fat_result_t fat_file_truncate(fat_file_t* f, uint64_t size);

// Write `len` bytes from `buf` at the cursor, extending the file and
// allocating clusters as needed; the cursor advances by *written. Like
// fat_file_read there are no short writes: FAT_OK means *written == len.
// On failure the file stays consistent with the prefix that landed
// (*written receives that byte count, dirent size included). Writes
// never create holes: the cursor cannot sit past the current size.
// Argument rules mirror fat_file_read exactly: a NULL buf is
// FAT_ERR_INVALID_ARG (even for len 0); len 0 with a valid buf is FAT_OK
// with *written == 0 and nothing touched.
fat_result_t fat_file_write(fat_file_t* f, const uint8_t* buf, size_t len,
                            size_t* written);

// Release the cursor. Safe on NULL. The context may outlive it.
void fat_file_close(fat_file_t* f);

// directory cursors ---------------------------------------------------------

// Opaque stepwise directory iterator. Skips deleted (0xE5) and LFN
// (attr 0x0F) entries exactly like fat_iter_dir.
typedef struct fat_dir fat_dir_t;

// Open `dir_cluster` (FAT_CLUSTER_ROOT for the root) for stepwise
// iteration. Validation matches fat_iter_dir.
fat_result_t fat_dir_open(fat_ctx_t* ctx, uint32_t dir_cluster,
                          fat_dir_t** out);

// Advance to the next entry.
//   FAT_OK             -> *out (and *raw32, unless NULL is passed) point
//                         into cursor-owned storage, valid until the next
//                         fat_dir_next/fat_dir_close
//   FAT_ERR_END_OF_DIR -> no more entries; the cursor stays exhausted and
//                         further calls keep returning FAT_ERR_END_OF_DIR
fat_result_t fat_dir_next(fat_dir_t* d, const fat_dirent_t** out,
                          const uint8_t** raw32);

// Release the cursor. Safe on NULL.
void fat_dir_close(fat_dir_t* d);

// timestamps ----------------------------------------------------------------

// Decode a DOS date/time pair (and the optional 0.01s field, pass 0 to
// ignore) into a broken-down calendar time. DOS epoch is 1980; the usable
// range is 1980-01-01..2107-12-31 (2-second resolution, +1/100s from
// dos_tenth). tm_isdst is 0; no timezone is claimed (interpret as UTC or
// local wall time as the caller prefers).
//   FAT_ERR_INVALID_ARG -> out is NULL, or the encoded fields are out of
//                          range (month 0/13+, day 0, hour 24+)
fat_result_t fat_dos_date_to_tm(uint16_t dos_date, uint16_t dos_time,
                                uint8_t dos_tenth, struct tm* out);

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

// write support -------------------------------------------------------------
// All write APIs mutate the sector cache owned by `ctx`; mutations reach
// the backend on fat_sync()/fat_close() (see the I/O abstraction below).
// The fat_open_mem backend is a private copy, so the caller's buffer is
// untouched either way. Read cursors (fat_file_t/fat_dir_t) observe the
// mutated cache, but keeping them open across structural changes (chain
// alloc/free, dirent writes) is undefined -- close them first. Write
// cursors (fat_file_open_write) are themselves structural: running two
// cursors over one file, or unlinking an open file, is the caller's
// responsibility and otherwise undefined.

// Set FAT[cluster] = value in every FAT copy the spec requires: all
// mirrors when mirroring is enabled, only the active copy when
// BPB_ExtFlags disables mirroring. `value` is the unpacked entry, at most
// 0xFFF / 0xFFFF / 0x0FFFFFFF by image type (larger is INVALID_ARG);
// FAT32 upper 4 reserved bits keep their on-disk value. `cluster` must be
// a valid data cluster (2..cluster_count+1).
fat_result_t fat_set_fat_entry(fat_ctx_t* ctx, uint32_t cluster,
                               uint32_t value);

// Find a free cluster, mark it EOC and return it via `out`. The FSInfo
// free count / next-free hint are kept in step (FAT32). The cluster's data
// bytes are NOT cleared -- zero them yourself when extending a directory.
//   FAT_ERR_DISK_FULL -> no free cluster remains
fat_result_t fat_alloc_cluster(fat_ctx_t* ctx, uint32_t* out);

// Free the whole chain headed at `head`: every entry becomes 0 in all
// required FAT copies, FSInfo counts are updated. A broken/looping chain
// frees the entries that were walked, then reports FAT_ERR_BAD_CLUSTER.
fat_result_t fat_free_chain(fat_ctx_t* ctx, uint32_t head);

// Create `name` in `dir_cluster` (FAT_CLUSTER_ROOT = the root directory),
// filling the new 32-byte entry from `tmpl` (attributes, timestamps,
// first_cluster, file_size; the on-disk name comes from `name` via
// fat_name_to_83). The first deleted (0xE5) slot is reused, else the
// first never-used (0x00) slot, else the directory chain is extended by
// one zeroed cluster (subdirectories and the FAT32 root only).
//   FAT_ERR_EXISTS        -> a live entry with this name already exists
//   FAT_ERR_NAME_TOO_LONG -> `name` is not representable in 8.3
//   FAT_ERR_DIR_FULL      -> FAT12/16 root region has no free slot
fat_result_t fat_add_dirent(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const fat_dirent_t* tmpl);

// Create a new file `name` in `dir_cluster` holding `size` bytes copied
// from `data` (data may be NULL only when size is 0). `tmpl` supplies the
// attributes and timestamps (NULL = ATTR_ARCHIVE and zero timestamps; its
// first_cluster/file_size fields are ignored). Crash-consistent order:
// allocate and fill the data chain first, write the dirent last; on any
// failure everything allocated so far is rolled back (freed) and the
// error is returned, leaving the directory and FAT as they were.
// An empty file gets first_cluster 0. An existing name is
// FAT_ERR_EXISTS -- replacing content is the write cursor's job
// (fat_file_open_write + fat_file_truncate / fat_file_write).
fat_result_t fat_write_file(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const uint8_t* data,
                            size_t size, const fat_dirent_t* tmpl);

// Delete the regular file `name` from `dir_cluster` (FAT_CLUSTER_ROOT =
// the root directory). Consistency order: the directory slot's first
// byte is set to 0xE5 first, then the file's cluster chain is freed
// (FSInfo kept in step) -- an interruption can leak clusters but never
// leaves a live entry pointing at freed ones. An empty file
// (first_cluster 0) just loses its entry.
//   FAT_ERR_NOT_FOUND   -> no live entry with this name
//   FAT_ERR_INVALID_ARG -> entry is a directory, or ATTR_READ_ONLY
fat_result_t fat_unlink(fat_ctx_t* ctx, uint32_t dir_cluster,
                        const char* name);

// Remove the empty directory `name` from `dir_cluster`. The target must
// be a directory holding no live entries besides "." and ".." (deleted
// 0xE5 slots do not block removal); the root directory itself cannot be
// removed. Same order as fat_unlink: slot marked 0xE5 first, then the
// chain freed.
//   FAT_ERR_NOT_FOUND       -> no live entry with this name
//   FAT_ERR_INVALID_ARG     -> entry is a regular file, is the root
//                              directory, or ATTR_READ_ONLY
//   FAT_ERR_DIR_NOT_EMPTY   -> target holds live entries
fat_result_t fat_rmdir(fat_ctx_t* ctx, uint32_t dir_cluster,
                       const char* name);

// Export the whole logical image (backend as seen through the cache) to
// `path` (created/truncated). This is not a cache flush to the backend --
// that is fat_sync().
fat_result_t fat_write(const fat_ctx_t* ctx, const char* path);

// I/O abstraction ------------------------------------------------------------
// Region accesses never touch a whole-image buffer: they go through a
// backend (fat_io_t) and a sector cache owned by the context. Writes are
// write-back: they live in the cache and are flushed to the backend by
// fat_sync()/fat_close() at the latest -- dirty sectors may be flushed
// earlier when the cache evicts them. Ship backends: RAM (fat_io_mem,
// used by fat_open_mem) and file (fat_io_file, used by fat_open). Embed
// fat_io_t in your own struct to mount block devices, FUSE, etc.

// Backend vtable. Embed it as the FIRST member of your struct; the
// library always calls e.g. io->read(io, ...), so a member can recover
// the outer struct with a plain downcast:
//   typedef struct { fat_io_t io; int fd; } my_io_t;
// Backends are fixed-size: size() must not change over the backend's
// lifetime. Transfers past size() fail with FAT_ERR_IO; zero-length
// transfers succeed. close() may be NULL (nothing to release); otherwise
// it is called exactly once, when the owning fat_ctx_t is closed.
typedef struct fat_io {
    fat_result_t (*read)(struct fat_io* io, uint64_t offset, void* buf,
                         size_t len);
    fat_result_t (*write)(struct fat_io* io, uint64_t offset,
                          const void* buf, size_t len);
    uint64_t (*size)(const struct fat_io* io);
    void (*close)(struct fat_io* io);
} fat_io_t;

// RAM backend over a private copy of `image` (the caller keeps the
// original buffer; library writes mutate the copy only).
fat_result_t fat_io_mem(fat_io_t** out, const uint8_t* image, size_t size);

// stdio file backend. The file must exist; it is opened read/write when
// permitted (else read-only, and writes fail with FAT_ERR_IO when dirty
// sectors are flushed). Library writes reach the file no later than
// fat_sync()/fat_close().
fat_result_t fat_io_file(fat_io_t** out, const char* path);

// Open through an explicit backend: validate the BPB and bind `*out`.
// Opening reads exactly one 512-byte sector (the boot sector) via the
// backend; a backend that cannot serve it fails with FAT_ERR_IO (a
// backend smaller than one sector therefore fails to open). On success
// the context owns `io` -- fat_close flushes dirty sectors and calls
// io->close exactly once. On failure the caller keeps ownership of `io`.
// Every other API behaves identically on top of any backend.
fat_result_t fat_open_io(fat_io_t* io, fat_ctx_t** out);

// Flush dirty cache sectors to the backend (NULL ctx is
// FAT_ERR_INVALID_ARG). Backend write failures surface here as
// FAT_ERR_IO (fat_close is best-effort: check errors via fat_sync
// first). The cache is coherent with itself only -- external backend
// modifications between calls are not picked up.
fat_result_t fat_sync(fat_ctx_t* ctx);

#endif
