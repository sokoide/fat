/* Phase 0 test suite: exercises the public fat.h API against the frozen
 * demof12.fat fixture (720KB FAT12, mtools-generated).  All fixture facts
 * asserted here were verified independently with mtools and raw image dumps:
 *
 *   root: DEMOF12 label (attr 0x08), HELLO.TXT (cluster 2, 12 bytes),
 *         TEST_5KB.TXT (cluster 3, 4962 bytes, chain 3->4->5->6->7),
 *         DIR1 (cluster 8), DIR2 (cluster 11, chain 11->43)
 *   dir1: ".", "..", SUBDIR1 (empty), SUBDIR2 (empty), HOGE.TXT (11 bytes)
 *   dir2: ".", "..", SUBDIR1..SUBDIR33 spread over clusters 11 and 43
 *   dir2/subdir1: PAGE.TXT (14 bytes), TEST_5KB.TXT (4962 bytes, chain
 *         48->49->50->51->52, byte-identical to the repo test_5kb.txt)
 *   FAT: [0]=0xFF9 [1]=0xFFF [2]=0xFFF [3]=4 [4]=5 [5]=6 [6]=7 [7]=0xFFF
 *        [8]=0xFFF [11]=0x02B [43]=0xFFF
 */

#include "fat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN(t)                                                  \
    do {                                                        \
        printf("%-40s", #t);                                    \
        fflush(stdout);                                         \
        (t)();                                                  \
        puts(" ok");                                            \
    } while (0)

#define IMG_NAME "demof12.fat"

/* BPB field offsets (fatgen103 layout, confirmed on the fixture bytes). */
#define BPB_BYTES_PER_SECTOR 11
#define BPB_RESERVED_SECTORS 14
#define BPB_TOTAL_SECTORS16  19
#define BPB_TOTAL_SECTORS32  32
#define BPB_SIGNATURE        510

/* Fixture geometry derived from the verified BPB (see test_open). */
#define FIXTURE_FAT_SECTOR   1u    /* reservedSectorCount */
#define FIXTURE_ROOT_SECTOR  7u    /* 1 + fatCount * fatSectors */
#define FIXTURE_ROOT_ENTRIES 112u  /* rootEntryCount 0x70 */

static fat_ctx_t* open_fixture(void)
{
    fat_ctx_t* ctx = NULL;
    fat_result_t r = fat_open(IMG_NAME, &ctx);
    if (r != FAT_OK)
        fprintf(stderr, "%s: %s\n", IMG_NAME, fat_strerror(r));
    assert(r == FAT_OK);
    assert(ctx != NULL);
    return ctx;
}

/* fat_get_fat_entry that insists on success and returns the value */
static uint32_t fat_at(const fat_ctx_t* ctx, uint32_t cluster)
{
    uint32_t v = 0;
    assert(fat_get_fat_entry(ctx, cluster, &v) == FAT_OK);
    return v;
}

/* ------------------------------------------------------------------ */
/* root directory iteration                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int labels;   /* volume labels (attr 0x08); LFN (0x0F) would land here too */
    int dirs;     /* subdirectories, dot entries excluded by the callback */
    int files;
    int raw32_missing;
    int saw_label, saw_hello, saw_5kb, saw_dir1, saw_dir2;
} RootCounts;

static void count_root(const fat_dirent_t* entry, const uint8_t* raw32,
                       void* user_data)
{
    RootCounts* counts = user_data;

    if (raw32 == NULL)
        counts->raw32_missing = 1;
    if (entry->attributes & 0x08) { /* volume label / LFN */
        counts->labels++;
        if (strcmp(entry->name, "DEMOF12") == 0)
            counts->saw_label = 1;
        return;
    }
    if (strcmp(entry->name, "HELLO.TXT") == 0) {
        counts->saw_hello = 1;
        assert(entry->first_cluster == 2);
        assert(entry->file_size == 12);
    } else if (strcmp(entry->name, "TEST_5KB.TXT") == 0) {
        counts->saw_5kb = 1;
        assert(entry->first_cluster == 3);
        assert(entry->file_size == 4962);
    } else if (strcmp(entry->name, "DIR1") == 0) {
        counts->saw_dir1 = 1;
        assert(entry->first_cluster == 8);
        assert(entry->attributes & 0x10);
    } else if (strcmp(entry->name, "DIR2") == 0) {
        counts->saw_dir2 = 1;
        assert(entry->first_cluster == 11);
        assert(entry->attributes & 0x10);
    }
    if (entry->attributes & 0x10)
        counts->dirs++;
    else
        counts->files++;
}

/* ------------------------------------------------------------------ */
/* dir2 iteration                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int dots;      /* "." and ".." (delivered by fat_iter_dir, not skipped) */
    int subdirs;
    int files;
    int bad_name;  /* non-dot entry not named SUBDIRn */
} Dir2Counts;

static void count_dir2(const fat_dirent_t* entry, const uint8_t* raw32,
                       void* user_data)
{
    Dir2Counts* counts = user_data;

    (void)raw32;
    if (entry->name[0] == '.') {
        counts->dots++;
        return;
    }
    if (strncmp(entry->name, "SUBDIR", 6) != 0)
        counts->bad_name = 1;
    if (entry->attributes & 0x10)
        counts->subdirs++;
    else
        counts->files++;
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void test_open(void)
{
    fat_ctx_t* ctx = NULL;

    /* a file that cannot be read is a documented I/O failure */
    assert(fat_open("no_such_image.fat", &ctx) == FAT_ERR_IO);

    ctx = open_fixture();

    /* geometry: fatStart=1, rootDir=1+2*3=7, rootDirSectors=ceil(112*32/512)=7,
     * dataStart=7+7=14, clusterCount=(0x5A0-14)/2=713 */
    const fat_geometry_t* g = fat_geometry(ctx);
    assert(g != NULL);
    assert(g->bytes_per_sector == 512);
    assert(g->sectors_per_cluster == 2);
    assert(g->reserved_sectors == 1);
    assert(g->fat_count == 2);
    assert(g->fat_sectors == 3);
    assert(g->root_entries == 0x70);
    assert(g->total_sectors == 0x5A0);
    assert(g->fat_start_sector == 1);
    assert(g->root_dir_sector == 7);
    assert(g->root_dir_sectors == 7);
    assert(g->data_start_sector == 14);
    assert(g->cluster_count == (0x5A0u - 14u) / 2u);
    assert(g->cluster_count == 713);

    assert(fat_get_type(ctx) == FT_FAT12);
    assert(fat_cluster_size(ctx) == 512u * 2u);

    assert(fat_strerror(FAT_OK) != NULL);
    assert(fat_strerror(FAT_ERR_BAD_CLUSTER) != NULL);

    fat_close(ctx);
}

static void test_fat_entries(void)
{
    fat_ctx_t* ctx = open_fixture();

    /* media/reserved entries and the verified chains */
    assert(fat_at(ctx, 0) == 0xFF9);
    assert(fat_at(ctx, 1) == 0xFFF);
    assert(fat_at(ctx, 2) == 0xFFF); /* hello.txt: single cluster, EOC */
    assert(fat_at(ctx, 3) == 4);     /* test_5kb.txt chain 3->4->5->6->7 */
    assert(fat_at(ctx, 4) == 5);
    assert(fat_at(ctx, 5) == 6);
    assert(fat_at(ctx, 6) == 7);
    assert(fat_at(ctx, 7) == 0xFFF);
    assert(fat_at(ctx, 8) == 0xFFF); /* dir1: single cluster */
    assert(fat_at(ctx, 11) == 0x02B); /* dir2: cluster 11 continues at 43 */
    assert(fat_at(ctx, 43) == 0xFFF);

    /* indices 0..cluster_count+1 are readable, beyond that is an error */
    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t v = 0;
    assert(fat_get_fat_entry(ctx, g->cluster_count + 1, &v) == FAT_OK);
    assert(fat_get_fat_entry(ctx, g->cluster_count + 2, &v) ==
           FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_root_iterate(void)
{
    fat_ctx_t* ctx = open_fixture();
    RootCounts counts = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    assert(fat_iter_dir(ctx, FAT_CLUSTER_ROOT, count_root, &counts) ==
           FAT_OK);
    /* DEMOF12 + hello.txt + test_5kb.txt + dir1 + dir2 */
    assert(counts.labels == 1);
    assert(counts.files == 2);
    assert(counts.dirs == 2);
    assert(counts.raw32_missing == 0);
    assert(counts.saw_label && counts.saw_hello && counts.saw_5kb);
    assert(counts.saw_dir1 && counts.saw_dir2);

    fat_close(ctx);
}

static void test_dir2_iterate(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de) == FAT_OK);
    assert(de.first_cluster == 11);

    /* cluster 11 (full, 32 slots) -> cluster 43: 33 subdirs, no files */
    Dir2Counts counts = {0, 0, 0, 0};
    assert(fat_iter_dir(ctx, de.first_cluster, count_dir2, &counts) ==
           FAT_OK);
    assert(counts.dots == 2);
    assert(counts.subdirs == 33);
    assert(counts.files == 0);
    assert(counts.bad_name == 0);

    fat_close(ctx);
}

static void assert_to_83(const char* in, const char* want11)
{
    uint8_t name11[11];
    assert(fat_name_to_83(in, name11) == FAT_OK);
    assert(memcmp(name11, want11, 11) == 0);
}

static void test_names(void)
{
    uint8_t name11[11];
    char out[FAT_NAME_MAX];

    /* to 8.3: uppercase, space padded; extension trailing spaces trimmed */
    assert_to_83("hoge", "HOGE       ");
    assert_to_83("page.txt", "PAGE    TXT");
    assert_to_83("foo.a", "FOO     A  ");
    assert_to_83("readme", "README     ");

    /* names not representable in 8.3 */
    assert(fat_name_to_83("toolongname.txt", name11) == FAT_ERR_NAME_TOO_LONG);
    assert(fat_name_to_83("file.text", name11) == FAT_ERR_NAME_TOO_LONG);
    /* rejected (fat.h does not pin the exact code for these three) */
    assert(fat_name_to_83("", name11) != FAT_OK);
    assert(fat_name_to_83(".", name11) != FAT_OK);
    assert(fat_name_to_83("..", name11) != FAT_OK);
    assert(fat_name_to_83(NULL, name11) != FAT_OK);

    /* from 8.3 round trip */
    assert(fat_name_to_83("page.txt", name11) == FAT_OK);
    assert(fat_name_from_83(name11, 0x20, out, sizeof(out)) == FAT_OK);
    assert(strcmp(out, "PAGE.TXT") == 0);

    /* directories get no extension dot */
    memcpy(name11, "SUBDIR1    ", 11);
    assert(fat_name_from_83(name11, 0x10, out, sizeof(out)) == FAT_OK);
    assert(strcmp(out, "SUBDIR1") == 0);

    /* volume labels get no extension dot either */
    memcpy(name11, "DEMOF12    ", 11);
    assert(fat_name_from_83(name11, 0x08, out, sizeof(out)) == FAT_OK);
    assert(strcmp(out, "DEMOF12") == 0);

    /* extension trailing spaces are trimmed, all-blank ext means no dot */
    memcpy(name11, "FOO     A  ", 11);
    assert(fat_name_from_83(name11, 0x20, out, sizeof(out)) == FAT_OK);
    assert(strcmp(out, "FOO.A") == 0);
    memcpy(name11, "README      ", 11);
    assert(fat_name_from_83(name11, 0x20, out, sizeof(out)) == FAT_OK);
    assert(strcmp(out, "README") == 0);

    /* output buffer must hold the result plus NUL ("HELLO.TXT" = 10) */
    memcpy(name11, "HELLO   TXT", 11);
    assert(fat_name_from_83(name11, 0x20, out, 9) == FAT_ERR_BUFFER_TOO_SMALL);
    assert(fat_name_from_83(name11, 0x20, out, 10) == FAT_OK);
    assert(strcmp(out, "HELLO.TXT") == 0);
}

static void test_lookup(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert(de.first_cluster == 8);
    assert(de.attributes & 0x10);

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de) == FAT_OK);
    assert(de.first_cluster == 11);

    /* leading slash and empty components are ignored */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/dir2//subdir1", &de) == FAT_OK);
    assert(de.first_cluster == 12);

    /* matching is case-insensitive (queries are folded to 8.3 upper) */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 12);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "HELLO.TXT", &de) == FAT_OK);
    assert(de.file_size == 12);

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    assert(de.first_cluster == 3 && de.file_size == 4962);

    /* deep paths, including the second copy inside dir2/subdir1 */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de) == FAT_OK);
    assert(de.file_size == 11);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/page.txt", &de) ==
           FAT_OK);
    assert(de.file_size == 14);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT,
                      "dir2/subdir1/test_5kb.txt", &de) == FAT_OK);
    assert(de.file_size == 4962);

    /* missing final component vs missing/not-a-directory intermediate */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/noexist", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "noexist/foo", &de) ==
           FAT_ERR_PATH_NOT_FOUND);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt/x", &de) ==
           FAT_ERR_PATH_NOT_FOUND);

    /* "." / ".." at the root: the fixed root region has no dot entries, so
     * neither can resolve to a dirent. fat_lookup handles both itself
     * (fat_name_to_83 rejects them); like "..", "." at the root fails with
     * PATH_NOT_FOUND -- there is no current-directory entry to stay in. */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, ".", &de) ==
           FAT_ERR_PATH_NOT_FOUND);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "..", &de) ==
           FAT_ERR_PATH_NOT_FOUND);

    /* "." resolves to the directory's own dot entry */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/.", &de) == FAT_OK);
    assert(de.first_cluster == 8);
    assert(de.attributes & 0x10);

    /* ".." of a root-level directory yields the root cluster */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/..", &de) == FAT_OK);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/../hello.txt", &de) ==
           FAT_OK);
    assert(de.file_size == 12);

    fat_close(ctx);
}

static void test_read_file(void)
{
    static uint8_t sentinel; /* non-NULL start catches "OK but out unwritten" */
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    uint8_t* data = NULL;
    size_t size = 0;

    /* multi-cluster chain: 4962 bytes over clusters 3->4->5->6->7 */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/test_5kb.txt", &de) == FAT_OK);
    data = &sentinel;
    size = 1;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_OK);
    assert(data != &sentinel);
    assert(size == 4962);
    {
        FILE* fp = fopen("test_5kb.txt", "rb");
        if (fp == NULL)
            perror("test_5kb.txt");
        assert(fp != NULL);
        uint8_t* expected = malloc(size);
        assert(expected != NULL);
        assert(fread(expected, 1, size, fp) == size);
        fclose(fp);
        assert(memcmp(data, expected, size) == 0);
        free(expected);
    }
    free(data);

    /* single-cluster files */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/hello.txt", &de) == FAT_OK);
    data = &sentinel;
    size = 1;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_OK);
    assert(size == 12);
    assert(memcmp(data, "hello world\n", 12) == 0);
    free(data);

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/dir1/hoge.txt", &de) == FAT_OK);
    data = &sentinel;
    size = 1;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_OK);
    assert(size == 11);
    assert(memcmp(data, "I am hoge.\n", 11) == 0);
    free(data);

    /* empty file (size 0, first cluster 0): no buffer, no error.  The
     * fixture has no empty file, so the dirent is fabricated. */
    memset(&de, 0, sizeof(de));
    de.attributes = 0x20;
    de.first_cluster = 0;
    de.file_size = 0;
    data = &sentinel;
    size = 1;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_OK);
    assert(data == NULL);
    assert(size == 0);

    /* directories and volume labels are not readable as files */
    de.attributes = 0x10;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_ERR_INVALID_ARG);
    de.attributes = 0x08;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_ctx_lifecycle(void)
{
    fat_ctx_t* a = NULL;
    fat_ctx_t* b = NULL;
    RootCounts ca = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    RootCounts cb = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    /* two independent contexts on the same image */
    a = open_fixture();
    b = open_fixture();
    assert(a != b);

    assert(fat_iter_dir(a, FAT_CLUSTER_ROOT, count_root, &ca) == FAT_OK);
    assert(fat_iter_dir(b, FAT_CLUSTER_ROOT, count_root, &cb) == FAT_OK);
    assert(ca.labels == 1 && ca.files == 2 && ca.dirs == 2);
    assert(cb.labels == 1 && cb.files == 2 && cb.dirs == 2);

    /* closing one context must not disturb the other */
    fat_close(a);
    memset(&cb, 0, sizeof(cb));
    assert(fat_iter_dir(b, FAT_CLUSTER_ROOT, count_root, &cb) == FAT_OK);
    assert(cb.labels == 1 && cb.files == 2 && cb.dirs == 2);
    fat_close(b);

    /* documented NULL handling */
    fat_close(NULL);
    assert(fat_get_type(NULL) == FT_UNKNOWN);
    assert(fat_geometry(NULL) == NULL);
}

/* ------------------------------------------------------------------ */
/* in-process mutated images (fat_open_mem)                            */
/* ------------------------------------------------------------------ */

static uint8_t* read_fixture(size_t* out_size)
{
    FILE* fp = fopen(IMG_NAME, "rb");
    if (fp == NULL)
        perror(IMG_NAME);
    assert(fp != NULL);
    assert(fseek(fp, 0, SEEK_END) == 0);
    long n = ftell(fp);
    assert(n > 0);
    assert(fseek(fp, 0, SEEK_SET) == 0);
    uint8_t* buf = malloc((size_t)n);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)n, fp) == (size_t)n);
    fclose(fp);
    *out_size = (size_t)n;
    return buf;
}

static uint64_t checksum(const uint8_t* p, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += p[i];
    return sum;
}

static uint8_t* copy_image(const uint8_t* src, size_t size)
{
    uint8_t* buf = malloc(size);
    assert(buf != NULL);
    memcpy(buf, src, size);
    return buf;
}

/* copy, patch [off, off+len) with val, return the mutated copy */
static uint8_t* mutated_copy(const uint8_t* src, size_t size, size_t off,
                             const void* val, size_t len)
{
    uint8_t* buf = copy_image(src, size);
    memcpy(buf + off, val, len);
    return buf;
}

/* Write a 12-bit FAT entry (1.5-byte little-endian packing) into the first
 * FAT table of the raw image. */
static void set_fat12_entry(uint8_t* img, uint32_t cluster, uint16_t value)
{
    size_t off = FIXTURE_FAT_SECTOR * 512u + cluster * 3u / 2u;
    if (cluster % 2u == 0u) {
        img[off] = (uint8_t)(value & 0xFFu);
        img[off + 1] =
            (uint8_t)((img[off + 1] & 0xF0u) | ((value >> 8) & 0x0Fu));
    } else {
        img[off] = (uint8_t)((img[off] & 0x0Fu) | ((value & 0x0Fu) << 4));
        img[off + 1] = (uint8_t)((value >> 4) & 0xFFu);
    }
}

/* open_mem must fail; a context accidentally bound on failure is released */
static void assert_open_mem_fails(const uint8_t* img, size_t size,
                                  fat_result_t want)
{
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == want);
    if (ctx != NULL)
        fat_close(ctx);
}

static void test_open_mem_errors(void)
{
    size_t size = 0;
    fat_ctx_t* ctx = NULL;
    uint8_t* orig = read_fixture(&size);
    uint64_t sum_before = checksum(orig, size);

    /* (a) boot signature 0x55AA destroyed */
    static const uint8_t sig_zero[2] = {0x00, 0x00};
    uint8_t* img = mutated_copy(orig, size, BPB_SIGNATURE, sig_zero, 2);
    assert_open_mem_fails(img, size, FAT_ERR_INVALID_BPB);
    free(img);

    /* (b) reservedSectorCount = 0xFFFF: geometry beyond the volume */
    static const uint8_t rsv_ff[2] = {0xFF, 0xFF};
    img = mutated_copy(orig, size, BPB_RESERVED_SECTORS, rsv_ff, 2);
    assert_open_mem_fails(img, size, FAT_ERR_INVALID_BPB);
    free(img);

    /* (c) totalSectors16 = 0 with totalSectors32 = 32768: a well-formed BPB
     * claiming (32768 - 14) / 2 = 16377 clusters, i.e. FAT16.  fat.h says
     * FT_FAT16 is "detected but rejected by fat_open", so the contract
     * points to FAT_ERR_UNSUPPORTED here. */
    static const uint8_t ts16_zero[2] = {0x00, 0x00};
    static const uint8_t ts32_16m[4] = {0x00, 0x80, 0x00, 0x00};
    img = mutated_copy(orig, size, BPB_TOTAL_SECTORS16, ts16_zero, 2);
    memcpy(img + BPB_TOTAL_SECTORS32, ts32_16m, 4);
    assert_open_mem_fails(img, size, FAT_ERR_UNSUPPORTED);
    free(img);

    /* (d) bytesPerSector = 768: not a legal power-of-two sector size */
    static const uint8_t bps768[2] = {0x00, 0x03};
    img = mutated_copy(orig, size, BPB_BYTES_PER_SECTOR, bps768, 2);
    assert_open_mem_fails(img, size, FAT_ERR_INVALID_BPB);
    free(img);

    /* (e) truncated image: not even a full boot sector */
    assert_open_mem_fails(orig, 100, FAT_ERR_INVALID_BPB);

    /* (f) inflated fileSize on TEST_5KBTXT: the chain 3->..->7 ends (EOC)
     * after 5120 bytes, far short of the claimed 1 MiB.  open must succeed
     * (only the BPB is validated); the read must refuse. */
    img = copy_image(orig, size);
    size_t root_off = FIXTURE_ROOT_SECTOR * 512u;
    size_t entry_off = 0;
    for (unsigned i = 0; i < FIXTURE_ROOT_ENTRIES; i++) {
        if (memcmp(img + root_off + i * 32u, "TEST_5KBTXT", 11) == 0) {
            entry_off = root_off + i * 32u;
            break;
        }
    }
    assert(entry_off != 0);
    static const uint8_t size_1m[4] = {0x00, 0x00, 0x10, 0x00}; /* LE 0x100000 */
    memcpy(img + entry_off + 28u, size_1m, 4);

    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img); /* the context owns its copy: using it past this free is the
                  point of the test */

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    assert(de.file_size == 0x100000);
    uint8_t* data = NULL;
    size_t n = 0;
    assert(fat_read_file(ctx, &de, &data, &n) == FAT_ERR_BAD_CLUSTER);
    assert(data == NULL); /* no buffer may escape on the error path */
    fat_close(ctx);
    ctx = NULL;

    /* (g) directory chain loop: FAT[11] = 11 makes dir2 point at itself.
     * Cluster 11 holds 32 fully-used entry slots (no 0x00 terminator), so
     * iteration must consult the FAT and trip the chain guard instead of
     * spinning forever. */
    img = copy_image(orig, size);
    set_fat12_entry(img, 11, 11);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    Dir2Counts counts = {0, 0, 0, 0};
    assert(fat_iter_dir(ctx, 11, count_dir2, &counts) == FAT_ERR_BAD_CLUSTER);
    fat_close(ctx);
    ctx = NULL;

    /* fat_open_mem copies the image: the pristine buffer survives every
     * mutation round trip untouched */
    assert(checksum(orig, size) == sum_before);
    free(orig);
}

int main(void)
{
    RUN(test_open);
    RUN(test_fat_entries);
    RUN(test_root_iterate);
    RUN(test_dir2_iterate);
    RUN(test_names);
    RUN(test_lookup);
    RUN(test_read_file);
    RUN(test_ctx_lifecycle);
    RUN(test_open_mem_errors);

    return 0;
}
