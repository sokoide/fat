// test_util.h -- the lightweight test framework and the helpers shared
// across the test groups. Each tests/test_*.c file defines its tests and
// ends with a register function (REGISTER / REGISTER_AS) that test_main.c
// calls; the runner gives every test its own failure isolation: a failed
// EXPECT prints the test name, file:line and both values, then abandons
// only that test and the suite continues.

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include "../fat.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fixture images live in the repository root (make check runs there) */
#define IMG_NAME "demof12.fat"
#define IMG16_NAME "demof16.fat"
#define IMG32_NAME "demof32.fat"

/* BPB field offsets (fatgen103 layout, confirmed on the fixture bytes) */
#define BPB_BYTES_PER_SECTOR 11
#define BPB_RESERVED_SECTORS 14
#define BPB_ROOT_ENTRIES 17
#define BPB_TOTAL_SECTORS16 19
#define BPB_TOTAL_SECTORS32 32
#define BPB_ROOT_CLUSTER 44
#define BPB_FSINFO_SECTOR 48
#define BPB_SIGNATURE 510

/* fixture geometry derived from the verified BPBs (see the tests) */
#define FIXTURE_FAT_SECTOR 1u   /* reservedSectorCount */
#define FIXTURE_ROOT_SECTOR 7u  /* 1 + fatCount * fatSectors */
#define FIXTURE_ROOT_ENTRIES 112u /* rootEntryCount 0x70 */
#define F16_FAT_SECTOR 1u /* demof16.fat: reservedSectorCount */
#define F32_FAT_SECTOR 32u /* demof32.fat: reservedSectorCount */

/* filler for output buffers before a call: unchanged canary bytes after
 * the call mean "the API wrote nothing here" (bounds/short-write checks) */
#define OUT_CANARY 0x5A5A

/* --- needs flags: the runner skips the test with a notice when unmet -- */

#define NEED_FAT16 1u /* demof16.fat must exist (make fat16) */
#define NEED_FAT32 2u /* demof32.fat must exist (make fat32) */
#define NEED_MTOOLS 4u /* mdir/mtype/mdel/mcopy must be installed */

typedef void (*test_fn)(void);

/* append one test to the run list (capacity is generous; order = run
 * order). Each test file ends with e.g.
 *   void test_read_register(void) {
 *       REGISTER(test_open);
 *       REGISTER_AS(test_fat16_open, NEED_FAT16);
 *   } */
void test_add(const char* name, test_fn fn, unsigned needs);
#define REGISTER(t) test_add(#t, (t), 0)
#define REGISTER_AS(t, needs) test_add(#t, (t), (needs))

/* run everything registered, in order; prints one line per test and
 * returns the number of failed tests (skips are not failures) */
int test_run_all(void);

/* --- failure isolation -------------------------------------------------
 * test_fail reports and longjmps out of the current test. The EXPECT
 * macros below are the only intended callers. */

void test_fail(const char* file, int line, const char* fmt, ...);

#define EXPECT_TRUE(cond)                                                    \
    do {                                                                     \
        if (!(cond))                                                         \
            test_fail(__FILE__, __LINE__, "EXPECT_TRUE(%s)", #cond);         \
    } while (0)

/* integer comparison (also pointers, via the cast); prints both values */
#define EXPECT_EQ(actual, want)                                              \
    do {                                                                     \
        long long va_ = (long long)(actual);                                 \
        long long vw_ = (long long)(want);                                   \
        if (va_ != vw_)                                                      \
            test_fail(__FILE__, __LINE__, "EXPECT_EQ(%s, %s): got %lld, "    \
                                        "want %lld",                         \
                      #actual, #want, va_, vw_);                              \
    } while (0)

#define EXPECT_STR_EQ(actual, want)                                          \
    do {                                                                     \
        if (strcmp((actual), (want)) != 0)                                   \
            test_fail(__FILE__, __LINE__, "EXPECT_STR_EQ(%s, %s): got "      \
                                        "\"%s\", want \"%s\"",               \
                      #actual, #want, (actual), (want));                     \
    } while (0)

#define EXPECT_MEMEQ(actual, want, n)                                        \
    do {                                                                     \
        if (memcmp((actual), (want), (n)) != 0)                              \
            test_fail(__FILE__, __LINE__, "EXPECT_MEMEQ(%s, %s, %zu): "      \
                                        "contents differ",                   \
                      #actual, #want, (size_t)(n));                           \
    } while (0)

/* --- callback bridge over the cursor API --------------------------------
 * fat_iter_dir left the public API (cursor-only iteration); this
 * cursor-driven loop keeps the suite's callback-style listings. */
typedef void (*iter_cb)(const fat_dirent_t* entry, const uint8_t* raw32,
                        void* user_data);
fat_result_t iter_dir(fat_ctx_t* ctx, uint32_t dir_cluster, iter_cb cb,
                      void* user_data);

/* --- image I/O and fixtures ---------------------------------------------- */

/* open a fixture by name; insists on success (a missing base fixture is
 * a broken build, not a skip) */
fat_ctx_t* open_image(const char* name);
fat_ctx_t* open_fixture(void);

/* 1 when the fixture image exists: missing FAT16/FAT32 images are a
 * skip (NEED_FAT16/32), not a failure */
int fixture_present(const char* name);

uint8_t* read_image(const char* name, size_t* out_size);
uint8_t* read_fixture(size_t* out_size);
void write_image(const char* name, const uint8_t* buf, size_t size);
uint8_t* copy_image(const uint8_t* src, size_t size);
/* copy, patch [off, off+len) with val, return the mutated copy */
uint8_t* mutated_copy(const uint8_t* src, size_t size, size_t off,
                      const void* val, size_t len);
uint64_t checksum(const uint8_t* p, size_t n);

/* raw-image FAT writers (first table, fatgen103 packing) */
void set_fat12_entry(uint8_t* img, uint32_t cluster, uint16_t value);
void set_fat32_entry(uint8_t* img, uint32_t cluster, uint32_t value);

/* fat_get_fat_entry that insists on success and returns the value */
uint32_t fat_at(const fat_ctx_t* ctx, uint32_t cluster);

/* open_mem must fail; a context accidentally bound on failure is released */
void assert_open_mem_fails(const uint8_t* img, size_t size, fat_result_t want);

/* deterministic write pattern (i*7+3) */
uint8_t* make_pattern(size_t n);

/* data clusters in the chain headed at `first` (stops at EOC/broken) */
uint32_t chain_length(const fat_ctx_t* ctx, uint32_t first);
/* free data clusters, by the FAT itself (ground truth for FSInfo checks) */
uint32_t count_free_clusters(const fat_ctx_t* ctx);
/* live entry count of a directory (label and dot entries included) */
int count_dir_entries(fat_ctx_t* ctx, uint32_t cluster);

/* After writing `name` into `dir_cluster`: lookup finds it, the dirent
 * size/first_cluster are exact, the FAT chain holds `want_clusters` data
 * clusters, and both fat_read_file and a fat_file_t cursor deliver the
 * original bytes. */
void assert_write_roundtrip(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const uint8_t* data, size_t size,
                            uint32_t want_clusters);
/* every geometry field must match (reopen comparisons) */
void assert_same_geometry(const fat_geometry_t* a, const fat_geometry_t* b);

/* --- content checks ------------------------------------------------------ */

/* fat_lookup + fat_read_file must deliver exactly `want` */
void assert_read_string(fat_ctx_t* ctx, const char* path, const char* want);
/* ... exactly the first want_size bytes of the host file */
void assert_read_matches_host(fat_ctx_t* ctx, const char* path,
                              const char* host_file, size_t want_size);

/* --- mtools oracle ------------------------------------------------------- */

/* 1 when mdir answers --version (probed once) */
int mtools_present(void);

#define MDIR_MAX 64
typedef struct {
    char name[16]; /* 8.3 = 12 chars max */
    int is_dir;
} MDirEntry;

/* run `mdir -b -i img ::dir`, parse lines into entries; -1 on mdir
 * failure (mtools was probed present, so that is a hard error) */
int run_mdir_b(const char* img_name, const char* dir, MDirEntry* out,
               int max);

/* the library's listing of `cluster` must equal mdir's listing of `dir` */
void assert_listing_matches_mdir(fat_ctx_t* ctx, const char* img_name,
                                 const char* dir, uint32_t cluster);
/* mtype's bytes for `path` must equal fat_read_file's */
void assert_mtype_matches_read(fat_ctx_t* ctx, const char* img_name,
                               const char* path);

#endif /* TEST_UTIL_H */
