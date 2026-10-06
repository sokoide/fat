/* Phase 2 test suite: exercises the public fat.h API against three frozen
 * mtools-generated fixtures.  All fixture facts asserted here were verified
 * independently with mtools (mdir/minfo) and raw image dumps (od):
 *
 * demof12.fat (720KB FAT12, mformat -f 720) -- committed to the repo:
 *   root: DEMOF12 label (attr 0x08), HELLO.TXT (cluster 2, 12 bytes),
 *         TEST_5KB.TXT (cluster 3, 4962 bytes, chain 3->4->5->6->7),
 *         DIR1 (cluster 8), DIR2 (cluster 11, chain 11->43)
 *   dir1: ".", "..", SUBDIR1 (empty, cluster 9), SUBDIR2 (empty, cluster
 *         10), HOGE.TXT (11 bytes, cluster 7)
 *   dir2: ".", "..", SUBDIR1..SUBDIR33 spread over clusters 11 and 43
 *   dir2/subdir1: PAGE.TXT (14 bytes), TEST_5KB.TXT (4962 bytes, chain
 *         48->49->50->51->52, byte-identical to the repo test_5kb.txt)
 *   FAT: [0]=0xFF9 [1]=0xFFF [2]=0xFFF [3]=4 [4]=5 [5]=6 [6]=7 [7]=0xFFF
 *        [8]=0xFFF [11]=0x02B [43]=0xFFF
 *
 * demof16.fat (16MiB FAT16, `make fat16`; gitignored) --
 *   geometry: bps 512, sectors/cluster 1, reserved 1, fats 2, fatSectors
 *         127, rootEntries 512, totalSectors16 32768 -> fatStart 1,
 *         rootDirSector 255, rootDirSectors 32, dataStart 287,
 *         clusterCount (32768-287)/1 = 32481 (FAT16 range [4085,65524])
 *   root: DEMOF16 label (attr 0x08), HELLO.TXT (cluster 2, 12 bytes),
 *         TEST_5KB.TXT (cluster 3, 4962 bytes, chain 3->4->..->12),
 *         DIR1 (cluster 13)
 *   dir1: ".", ".."(->0), SUB1 (cluster 14), HOGE.TXT (cluster 16, 11 bytes)
 *   sub1: ".", ".."(->13), PAGE.TXT (cluster 15, 14 bytes)
 *   FAT: [0]=0xFFF8 [1]=0xFFFF [2]=0xFFFF [3]=4 ... [11]=12 [12]=0xFFFF
 *        [13]=0xFFFF [14]=0xFFFF [15]=0xFFFF [16]=0xFFFF
 *
 * demof32.fat (33MiB FAT32, `make fat32`; gitignored) --
 *   geometry: bps 512, sectors/cluster 1, reserved 32, fats 2, fatSectors
 *         (tableSize32) 520, rootEntries 0, totalSectors32 67584 ->
 *         fatStart 32, rootDirSector/rootDirSectors 0, rootCluster 2,
 *         dataStart 1072, clusterCount (67584-1072)/1 = 66512 (>= 65525)
 *   root chain: 2 -> 54 -> 55 (44 entries over 3 clusters):
 *         cluster 2: DEMOF32 label (attr 0x08), HELLO.TXT (cluster 3,
 *         12 bytes), TEST_5KB.TXT (cluster 4, 4962 bytes, chain
 *         4->5->..->13), F00.TXT..F12.TXT (clusters 14..26)
 *         cluster 54: F13.TXT..F28.TXT (clusters 27..42)
 *         cluster 55: F29.TXT..F39.TXT (clusters 43..53), DIR1 (cluster 56)
 *   F-files: "filler file N" content, 14 bytes for N<10 else 15 bytes
 *   dir1 (cluster 56): ".", ".."(->0), SUB1 (cluster 57), HOGE.TXT
 *         (cluster 59, 11 bytes)
 *   sub1 (cluster 57): ".", ".."(->56), PAGE.TXT (cluster 58, 14 bytes)
 *   FAT (masked 32-bit values): [0]=0x0FFFFFF8 [1]=0x0FFFFFFF [2]=54
 *        [3]=0x0FFFFFFF [4]=5 ... [12]=13 [13]=0x0FFFFFFF [14..26]=EOC
 *        [27..53]=EOC [54]=55 [55]=0x0FFFFFFF [56..59]=EOC
 *   FSInfo (sector 1, per BPB@48): lead "RRaA", freeClusterCount 66454,
 *         nextFreeCluster 59; mdir cross-check: 34024448 bytes free
 *         = 66454 * 512 exactly
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
#define IMG16_NAME "demof16.fat"
#define IMG32_NAME "demof32.fat"

/* BPB field offsets (fatgen103 layout, confirmed on the fixture bytes). */
#define BPB_BYTES_PER_SECTOR 11
#define BPB_RESERVED_SECTORS 14
#define BPB_ROOT_ENTRIES     17
#define BPB_TOTAL_SECTORS16  19
#define BPB_TOTAL_SECTORS32  32
#define BPB_ROOT_CLUSTER     44
#define BPB_FSINFO_SECTOR    48
#define BPB_SIGNATURE        510

/* Fixture geometry derived from the verified BPBs (see the tests). */
#define FIXTURE_FAT_SECTOR   1u    /* reservedSectorCount */
#define FIXTURE_ROOT_SECTOR  7u    /* 1 + fatCount * fatSectors */
#define FIXTURE_ROOT_ENTRIES 112u  /* rootEntryCount 0x70 */
#define F16_FAT_SECTOR       1u    /* demof16.fat: reservedSectorCount */
#define F32_FAT_SECTOR       32u   /* demof32.fat: reservedSectorCount */

static fat_ctx_t* open_image(const char* name)
{
    fat_ctx_t* ctx = NULL;
    fat_result_t r = fat_open(name, &ctx);
    if (r != FAT_OK)
        fprintf(stderr, "%s: %s\n", name, fat_strerror(r));
    assert(r == FAT_OK);
    assert(ctx != NULL);
    return ctx;
}

static fat_ctx_t* open_fixture(void)
{
    return open_image(IMG_NAME);
}

/* 1 when the fixture image exists: missing FAT16/FAT32 suites are skipped
 * with a notice instead of failing (plain `make check` stays fast) */
static int fixture_present(const char* name)
{
    FILE* fp = fopen(name, "rb");
    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
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
    assert(g->root_cluster == 0); /* FAT12/16: fixed root region, no chain */
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

/* A1: on-disk first byte 0x05 is the escape for a true first byte 0xE5
 * (a name whose real lead byte is 0xE5 would be mistaken for a deleted
 * entry, so FAT stores it as 0x05).  Both stored forms must render the
 * same name, with a literal 0xE5 first byte.  High (non-ASCII) bytes are
 * copied through verbatim; blank extension on a file means no dot. */
static void test_name_05_escape(void)
{
    static const uint8_t stored05[11] = {
        0x05, 0x81, 0x90, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    static const uint8_t storedE5[11] = {
        0xE5, 0x81, 0x90, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    static const uint8_t want[4] = {0xE5, 0x81, 0x90, 0x00};
    char out05[FAT_NAME_MAX];
    char outE5[FAT_NAME_MAX];

    assert(fat_name_from_83(stored05, 0x20, out05, sizeof(out05)) == FAT_OK);
    assert(fat_name_from_83(storedE5, 0x20, outE5, sizeof(outE5)) == FAT_OK);
    /* byte-exact render (0xE5 lead), and both forms agree */
    assert(memcmp(out05, want, sizeof(want)) == 0);
    assert(memcmp(outE5, want, sizeof(want)) == 0);
    assert(memcmp(out05, outE5, sizeof(want)) == 0);
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

    /* A2: "." / ".." at the root now succeed with a synthetic root dirent
     * (DOS semantics: the root is its own parent).  The fixed root region
     * has no dot entries; fat_lookup must synthesize one. */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, ".", &de) == FAT_OK);
    assert(strcmp(de.name, ".") == 0);
    assert(de.attributes & 0x10);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(de.file_size == 0);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "..", &de) == FAT_OK);
    assert(strcmp(de.name, "..") == 0);
    assert(de.attributes & 0x10);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(de.file_size == 0);

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

/* A2: the root is its own parent, transitively -- any number of ".." from
 * the root stays at the root, and paths through them keep resolving. */
static void test_lookup_root_dots(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    /* leading/trailing and repeated root dots */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/.", &de) == FAT_OK);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "./..", &de) == FAT_OK);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);

    /* down and back up past the root: dir1/.. lands on the root, and the
     * next ".." must stay there instead of failing */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/../..", &de) == FAT_OK);
    assert(strcmp(de.name, "..") == 0);
    assert(de.attributes & 0x10);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(de.file_size == 0);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/../../hello.txt", &de) ==
           FAT_OK);
    assert(de.file_size == 12);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "../../dir1/hoge.txt", &de) ==
           FAT_OK);
    assert(de.file_size == 11);

    fat_close(ctx);
}

/* A3: volume-label entries (attr 0x08) are metadata, not openable objects;
 * fat_lookup must never match them (DOS open() semantics).  Iteration
 * still shows them (test_root_iterate). */
static void test_lookup_volume_label(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "demof12", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "DEMOF12", &de) ==
           FAT_ERR_NOT_FOUND);

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

static uint8_t* read_image(const char* name, size_t* out_size)
{
    FILE* fp = fopen(name, "rb");
    if (fp == NULL)
        perror(name);
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

static uint8_t* read_fixture(size_t* out_size)
{
    return read_image(IMG_NAME, out_size);
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

/* Write a 32-bit FAT entry (little-endian) into the first FAT table of the
 * raw FAT32 image. */
static void set_fat32_entry(uint8_t* img, uint32_t cluster, uint32_t value)
{
    size_t off = F32_FAT_SECTOR * 512u + (size_t)cluster * 4u;
    img[off] = (uint8_t)(value & 0xFFu);
    img[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    img[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    img[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
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
     * claiming (32768 - 14) / 2 = 16377 clusters, i.e. FAT16 (FATSz16 and
     * rootEntryCount are nonzero, so this stays in the FAT12/16 family).
     * Phase 2 fat_open accepts FAT16, so the type alone no longer rejects
     * this; what still must is the region fit: the claimed volume spans
     * 32768 * 512 = 16MiB while the buffer holds only 720KB.  A FAT table
     * too small for the claimed cluster count is NOT an error by itself
     * (nothing validates FAT coverage), only the image-size fit is.
     * Confirmed against the phase-2 fat_core.c: the phase-1 short-circuit
     * "ts16==0 -> UNSUPPORTED" is gone (FAT32 images are all ts16==0) and
     * the whole-volume-must-fit check fires first. */
    static const uint8_t ts16_zero[2] = {0x00, 0x00};
    static const uint8_t ts32_16m[4] = {0x00, 0x80, 0x00, 0x00};
    img = mutated_copy(orig, size, BPB_TOTAL_SECTORS16, ts16_zero, 2);
    memcpy(img + BPB_TOTAL_SECTORS32, ts32_16m, 4);
    assert_open_mem_fails(img, size, FAT_ERR_INVALID_BPB);
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

/* ---- B6/B7/B8 (link-red until implemented) ---- */

/* B8: DOS date/time decode helpers.
 * date = ((year-1980)<<9) | (month<<5) | day, time = (h<<11)|(min<<5)|s/2 */
static uint16_t dos_date_bits(int year, int month, int day)
{
    return (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

static uint16_t dos_time_bits(int hour, int minute, int second)
{
    return (uint16_t)((hour << 11) | (minute << 5) | (second / 2));
}

static void assert_tm_fields(const struct tm* tm, int year, int mon, int mday,
                             int hour, int min, int sec)
{
    assert(tm->tm_year == year);
    assert(tm->tm_mon == mon);
    assert(tm->tm_mday == mday);
    assert(tm->tm_hour == hour);
    assert(tm->tm_min == min);
    assert(tm->tm_sec == sec);
    assert(tm->tm_isdst == 0);
}

static void test_dos_date_to_tm(void)
{
    struct tm tm;

    /* DOS epoch: 1980-01-01 00:00:00 */
    memset(&tm, 0xAA, sizeof(tm));
    assert(fat_dos_date_to_tm(dos_date_bits(1980, 1, 1),
                              dos_time_bits(0, 0, 0), 0, &tm) == FAT_OK);
    assert_tm_fields(&tm, 80, 0, 1, 0, 0, 0);

    /* max encodable timestamp: 2107-12-31 23:59:58 */
    memset(&tm, 0xAA, sizeof(tm));
    assert(fat_dos_date_to_tm(dos_date_bits(2107, 12, 31),
                              dos_time_bits(23, 59, 58), 0, &tm) == FAT_OK);
    assert_tm_fields(&tm, 207, 11, 31, 23, 59, 58);

    /* ordinary stamp with a 0.01s field: struct tm has no sub-second slot,
     * so the tenth must not disturb any decoded field.  fat.h only
     * promises the date/time decode ("pass 0 to ignore"), so this asserts
     * exactly that much: tenth=78 decodes identically to tenth=0. */
    memset(&tm, 0xAA, sizeof(tm));
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                              dos_time_bits(12, 34, 56), 78, &tm) == FAT_OK);
    assert_tm_fields(&tm, 126, 9, 5, 12, 34, 56);
    memset(&tm, 0xAA, sizeof(tm));
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                              dos_time_bits(12, 34, 56), 0, &tm) == FAT_OK);
    assert_tm_fields(&tm, 126, 9, 5, 12, 34, 56);

    /* out-of-range encoded fields (fat.h: month 0/13+, day 0, hour 24+) */
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 0, 5),
                              dos_time_bits(12, 34, 56), 0, &tm) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 13, 5),
                              dos_time_bits(12, 34, 56), 0, &tm) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 10, 0),
                              dos_time_bits(12, 34, 56), 0, &tm) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                              dos_time_bits(24, 0, 0), 0, &tm) ==
           FAT_ERR_INVALID_ARG);

    /* NULL out */
    assert(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                              dos_time_bits(12, 34, 56), 0, NULL) ==
           FAT_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ */
/* B6: fat_file_t streaming reads vs fat_read_file ground truth        */
/* ------------------------------------------------------------------ */

/* Exercise one file through the whole fat_file_t surface and compare
 * every byte with fat_read_file's whole-file buffer. */
static void exercise_file_stream(fat_ctx_t* ctx, const char* path)
{
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de) == FAT_OK);

    uint8_t* whole = NULL;
    size_t whole_size = 0;
    assert(fat_read_file(ctx, &de, &whole, &whole_size) == FAT_OK);
    assert(whole_size == de.file_size);

    fat_file_t* f = NULL;
    assert(fat_file_open(ctx, &de, &f) == FAT_OK);
    assert(f != NULL);
    assert(fat_file_size(f) == whole_size);
    assert(fat_file_tell(f) == 0);

    /* whole file in a single read: full length delivered (a short read
     * only happens at EOF), bytes identical, cursor at the end */
    if (whole_size > 0) {
        uint8_t* buf = malloc(whole_size);
        assert(buf != NULL);
        size_t got = 0x5A5A;
        assert(fat_file_read(f, buf, whole_size, &got) == FAT_OK);
        assert(got == whole_size);
        assert(memcmp(buf, whole, whole_size) == 0);
        assert(fat_file_tell(f) == whole_size);
        /* at EOF further reads deliver 0 bytes with FAT_OK */
        got = 0x5A5A;
        assert(fat_file_read(f, buf, 16, &got) == FAT_OK);
        assert(got == 0);
        free(buf);
    }

    /* 1-byte chunked walk over the first 2000 bytes (FAT12: crosses the
     * first cluster boundary at 1024); tell tracks every single step */
    {
        size_t steps = whole_size < 2000 ? whole_size : 2000;
        for (size_t i = 0; i < steps; i++) {
            uint8_t b = 0;
            size_t got = 0;
            assert(fat_file_seek(f, i) == FAT_OK);
            assert(fat_file_tell(f) == i);
            assert(fat_file_read(f, &b, 1, &got) == FAT_OK);
            assert(got == 1);
            assert(b == whole[i]);
            assert(fat_file_tell(f) == i + 1);
        }
    }

    /* seek edges: 0, EOF (legal), and one past the end */
    assert(fat_file_seek(f, 0) == FAT_OK);
    assert(fat_file_tell(f) == 0);
    assert(fat_file_seek(f, whole_size) == FAT_OK);
    assert(fat_file_tell(f) == whole_size);
    {
        uint8_t b = 0;
        size_t got = 0x5A5A;
        assert(fat_file_read(f, &b, 1, &got) == FAT_OK);
        assert(got == 0);
    }
    assert(fat_file_seek(f, whole_size + 1) == FAT_ERR_INVALID_ARG);
    if (whole_size > 0) {
        /* short read ONLY at EOF: 2 bytes requested at size-1 deliver 1 */
        uint8_t b[2] = {0, 0};
        size_t got = 0x5A5A;
        assert(fat_file_seek(f, whole_size - 1) == FAT_OK);
        assert(fat_file_read(f, b, 2, &got) == FAT_OK);
        assert(got == 1);
        assert(b[0] == whole[whole_size - 1]);
    }
    /* read spanning a cluster boundary: last byte of cluster 0 and first
     * byte of cluster 1 in one call */
    {
        uint32_t cs = fat_cluster_size(ctx);
        if (whole_size > cs) {
            uint8_t b[2] = {0, 0};
            size_t got = 0;
            assert(fat_file_seek(f, cs - 1) == FAT_OK);
            assert(fat_file_read(f, b, 2, &got) == FAT_OK);
            assert(got == 2);
            assert(b[0] == whole[cs - 1] && b[1] == whole[cs]);
        }
    }

    fat_file_close(f);
    free(whole);
}

static void test_file_stream(void)
{
    fat_ctx_t* ctx = open_fixture();

    /* multi-cluster (5 clusters of 1024B) and single-cluster files */
    exercise_file_stream(ctx, "test_5kb.txt");
    exercise_file_stream(ctx, "hello.txt");

    /* empty file: fabricated dirent (the fixture has no empty file) */
    {
        fat_dirent_t de;
        memset(&de, 0, sizeof(de));
        de.attributes = 0x20;
        de.first_cluster = 0;
        de.file_size = 0;
        fat_file_t* f = NULL;
        assert(fat_file_open(ctx, &de, &f) == FAT_OK);
        assert(fat_file_size(f) == 0);
        assert(fat_file_tell(f) == 0);
        uint8_t b = 0;
        size_t got = 0x5A5A;
        assert(fat_file_read(f, &b, 8, &got) == FAT_OK);
        assert(got == 0);
        assert(fat_file_seek(f, 0) == FAT_OK);
        assert(fat_file_seek(f, 1) == FAT_ERR_INVALID_ARG); /* past size 0 */
        fat_file_close(f);
    }

    /* non-file dirents are rejected, same rules as fat_read_file */
    {
        fat_dirent_t de;
        fat_file_t* f = NULL;
        memset(&de, 0, sizeof(de));
        de.attributes = 0x10; /* directory */
        de.first_cluster = 8;
        assert(fat_file_open(ctx, &de, &f) == FAT_ERR_INVALID_ARG);
        de.attributes = 0x08; /* volume label */
        assert(fat_file_open(ctx, &de, &f) == FAT_ERR_INVALID_ARG);
    }

    /* documented NULL handling */
    fat_file_close(NULL);
    assert(fat_file_tell(NULL) == 0);
    assert(fat_file_size(NULL) == 0);

    fat_close(ctx);
}

static void test_fat16_file_stream(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);

    /* 4962 bytes over 512B clusters (chain 3->4->..->12) + single cluster */
    exercise_file_stream(ctx, "test_5kb.txt");
    exercise_file_stream(ctx, "hello.txt");

    fat_close(ctx);
}

static void test_fat32_file_stream(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);

    /* 4962 bytes over 512B clusters (chain 4->5->..->13) + single cluster */
    exercise_file_stream(ctx, "test_5kb.txt");
    exercise_file_stream(ctx, "hello.txt");

    fat_close(ctx);

    /* broken chain: FAT[5]=5 self-loop inside the test_5kb chain.
     * open must succeed (only the BPB is validated); the read must trip
     * the same chain guard as fat_read_file (Brent) -> BAD_CLUSTER */
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    set_fat32_entry(img, 5, 5);
    fat_ctx_t* mctx = NULL;
    assert(fat_open_mem(img, size, &mctx) == FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(mctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    fat_file_t* f = NULL;
    assert(fat_file_open(mctx, &de, &f) == FAT_OK);
    uint8_t* buf = malloc(de.file_size);
    assert(buf != NULL);
    size_t got = 0x5A5A;
    assert(fat_file_read(f, buf, de.file_size, &got) == FAT_ERR_BAD_CLUSTER);
    free(buf);
    fat_file_close(f);
    fat_close(mctx);
    free(orig);
}

/* ------------------------------------------------------------------ */
/* B7: fat_dir_t cursors vs fat_iter_dir                               */
/* ------------------------------------------------------------------ */

#define COLLECT_MAX 64 /* biggest listing: the 44-entry FAT32 root */

typedef struct {
    fat_dirent_t e[COLLECT_MAX];
    uint8_t raw[COLLECT_MAX][32];
    int n;
    int overflow;
} EntryList;

static void collect_entries(const fat_dirent_t* entry, const uint8_t* raw32,
                            void* user_data)
{
    EntryList* list = user_data;

    if (list->n >= COLLECT_MAX) {
        list->overflow = 1;
        return;
    }
    list->e[list->n] = *entry;
    memcpy(list->raw[list->n], raw32, 32);
    list->n++;
}

/* The cursor must enumerate exactly what fat_iter_dir enumerates on the
 * same cluster -- same entries in the same order -- then report
 * FAT_ERR_END_OF_DIR, stickily. */
static void assert_dir_cursor_matches_iter(fat_ctx_t* ctx, uint32_t cluster)
{
    EntryList list;
    memset(&list, 0, sizeof(list));
    assert(fat_iter_dir(ctx, cluster, collect_entries, &list) == FAT_OK);
    assert(list.overflow == 0);

    fat_dir_t* d = NULL;
    assert(fat_dir_open(ctx, cluster, &d) == FAT_OK);
    assert(d != NULL);
    for (int i = 0; i < list.n; i++) {
        const fat_dirent_t* e = NULL;
        const uint8_t* r = NULL;
        assert(fat_dir_next(d, &e, &r) == FAT_OK);
        assert(e != NULL && r != NULL);
        assert(strcmp(e->name, list.e[i].name) == 0);
        assert(e->attributes == list.e[i].attributes);
        assert(e->first_cluster == list.e[i].first_cluster);
        assert(e->file_size == list.e[i].file_size);
        /* raw32: the same 32 on-disk bytes, and their first 11 must be
         * the 8.3 encoding of the rendered name (dot entries are not 8.3
         * names, so those skip the round trip) */
        assert(memcmp(r, list.raw[i], 32) == 0);
        if (e->name[0] != '.') {
            uint8_t name11[11];
            assert(fat_name_to_83(e->name, name11) == FAT_OK);
            assert(memcmp(r, name11, 11) == 0);
        }
    }
    const fat_dirent_t* e = NULL;
    const uint8_t* r = NULL;
    assert(fat_dir_next(d, &e, &r) == FAT_ERR_END_OF_DIR);
    assert(fat_dir_next(d, &e, &r) == FAT_ERR_END_OF_DIR); /* sticky */
    fat_dir_close(d);
}

static void test_dir_cursor(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_dir_cursor_matches_iter(ctx, FAT_CLUSTER_ROOT);

    /* dir2: multi-cluster chain 11->43, 35 entries (2 dots + 33 subdirs) */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de) == FAT_OK);
    assert_dir_cursor_matches_iter(ctx, de.first_cluster);

    /* a file's cluster is not a directory: hello.txt lives in cluster 2
     * (its data "hello world" does not start with a "." entry like every
     * real subdirectory does); cluster 1 is a reserved FAT entry */
    fat_dir_t* d = NULL;
    assert(fat_dir_open(ctx, 2, &d) == FAT_ERR_INVALID_ARG);
    assert(fat_dir_open(ctx, 1, &d) == FAT_ERR_INVALID_ARG);

    fat_dir_close(NULL); /* documented NULL safety */

    fat_close(ctx);
}

static void test_fat16_dir_cursor(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_dir_cursor_matches_iter(ctx, FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert_dir_cursor_matches_iter(ctx, de.first_cluster);

    fat_close(ctx);
}

static void test_fat32_dir_cursor(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    /* root: the 3-cluster chain 2->54->55 with 44 entries */
    assert_dir_cursor_matches_iter(ctx, FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert_dir_cursor_matches_iter(ctx, de.first_cluster);

    fat_close(ctx);
}

/* ------------------------------------------------------------------ */
/* A4: boot-sector fuzz (regression: pass = no crash/hang/ASan report) */
/* ------------------------------------------------------------------ */

static void fuzz_noop_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    (void)entry;
    (void)raw32;
    (void)user_data;
}

/* full pipeline on one mutated image: open -> root iterate -> lookup ->
 * whole-file read of test_5kb.txt (fat_read_file has no length bound;
 * the 4962-byte fixture bounds it).  Any fat_result_t is acceptable at
 * every step; surviving is the contract.  *lookup_read counts full
 * pipeline runs that reached a successful read. */
static fat_result_t fuzz_pipeline(const uint8_t* img, size_t size,
                                  int* lookup_read)
{
    fat_ctx_t* ctx = NULL;
    fat_dirent_t de;

    *lookup_read = 0;
    fat_result_t r = fat_open_mem(img, size, &ctx);
    if (r != FAT_OK) {
        if (ctx != NULL)
            fat_close(ctx); /* a context bound on failure is released */
        return r;
    }
    (void)fat_iter_dir(ctx, FAT_CLUSTER_ROOT, fuzz_noop_cb, NULL);
    if (fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK) {
        uint8_t* data = NULL;
        size_t n = 0;
        if (fat_read_file(ctx, &de, &data, &n) == FAT_OK)
            *lookup_read = 1;
        free(data);
    }
    fat_close(ctx);
    return r;
}

static void test_boot_fuzz_fat12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    int opened = 0, rejected = 0, read_ok = 0;

    /* exhaustive: every bit of all 512 boot-sector bytes = 4096 mutants.
     * Measured ~0.1ms per mutant native (~0.4s total, a few seconds under
     * ASan) -- no reduction needed on this device. */
    for (size_t byte = 0; byte < 512; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            int lr = 0;
            memcpy(img, orig, size);
            img[byte] ^= (uint8_t)(1u << bit);
            if (fuzz_pipeline(img, size, &lr) == FAT_OK)
                opened++;
            else
                rejected++;
            read_ok += lr;
        }
    }
    /* determinism guard: both outcomes and the full pipeline must have
     * actually run (BPB flips reject; OEM-name flips open fine) */
    assert(opened > 0);
    assert(rejected > 0);
    assert(read_ok > 0);

    free(img);
    free(orig);
}

struct bpb_field {
    size_t off;
    size_t len;
};

/* critical BPB offsets (fatgen103 layout) and their byte widths */
static const struct bpb_field BPB_FIELDS_16[] = {
    {11, 2}, /* bytesPerSector */
    {13, 1}, /* sectorsPerCluster */
    {14, 2}, /* reservedSectorCount */
    {16, 1}, /* tableCount */
    {17, 2}, /* rootEntryCount */
    {19, 2}, /* totalSectors16 */
    {21, 1}, /* mediaType */
    {22, 2}, /* tableSize16 */
    {32, 4}, /* totalSectors32 */
};
static const struct bpb_field BPB_FIELDS_32[] = {
    {11, 2}, {13, 1}, {14, 2}, {16, 1}, {17, 2},
    {19, 2}, {21, 1}, {22, 2}, {32, 4},
    {36, 4}, /* FAT32 tableSize32 */
    {40, 2}, /* FAT32 extFlags */
    {44, 4}, /* FAT32 rootCluster */
    {48, 2}, /* FAT32 fsInfo sector */
};

static void fuzz_bpb_values(const char* img_name,
                            const struct bpb_field* fields, size_t nfields)
{
    size_t size = 0;
    uint8_t* orig = read_image(img_name, &size);
    uint8_t* img = copy_image(orig, size);
    int opened = 0, rejected = 0, read_ok = 0;
    static const uint8_t patterns[3] = {0x00, 0xFF, 0x55};

    /* each critical field x {0x00, 0xFF, 0x55} byte-fill (0x5555... for
     * multi-byte fields).  Copying 16/33MiB per mutant bounds this to the
     * value grid -- a full bit sweep is not affordable at these sizes. */
    for (size_t i = 0; i < nfields; i++) {
        for (int p = 0; p < 3; p++) {
            int lr = 0;
            memcpy(img, orig, size);
            memset(img + fields[i].off, patterns[p], fields[i].len);
            if (fuzz_pipeline(img, size, &lr) == FAT_OK)
                opened++;
            else
                rejected++;
            read_ok += lr;
        }
    }
    assert(opened > 0);
    assert(rejected > 0);
    assert(read_ok > 0);

    free(img);
    free(orig);
}

static void test_boot_fuzz_values_fat16(void)
{
    fuzz_bpb_values(IMG16_NAME, BPB_FIELDS_16,
                    sizeof(BPB_FIELDS_16) / sizeof(BPB_FIELDS_16[0]));
}

static void test_boot_fuzz_values_fat32(void)
{
    fuzz_bpb_values(IMG32_NAME, BPB_FIELDS_32,
                    sizeof(BPB_FIELDS_32) / sizeof(BPB_FIELDS_32[0]));
}

/* ------------------------------------------------------------------ */
/* A5: mtools differential (mdir -b / mtype as the oracle)             */
/* ------------------------------------------------------------------ */

/* mtools 4.0.48 `mdir -b` format, verified empirically on this device:
 * one entry per line as "::/dir/name" -- the requested directory is
 * repeated in each line, directories carry a trailing '/', names are
 * printed in lowercase (we render uppercase), and neither the volume
 * label nor "."/".." entries are listed.  No sizes are printed; content
 * equality is covered by mtype instead. */

static int mtools_probe_state = -1;

static int mtools_present(void)
{
    if (mtools_probe_state < 0) {
        FILE* p = popen("mdir --version 2>/dev/null", "r");
        char buf[64];
        int ok = p != NULL && fgets(buf, sizeof(buf), p) != NULL;
        if (p != NULL)
            pclose(p);
        mtools_probe_state = ok ? 1 : 0;
    }
    return mtools_probe_state;
}

#define MDIR_MAX 64

typedef struct {
    char name[16]; /* 8.3 = 12 chars max */
    int is_dir;
} MDirEntry;

static int upper_eq(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a >= 'a' && *a <= 'z' ? (char)(*a - 'a' + 'A') : *a;
        char cb = *b >= 'a' && *b <= 'z' ? (char)(*b - 'a' + 'A') : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* run `mdir -b -i img ::dir`, parse lines into entries; -1 on mdir
 * failure (mtools was probed present, so that is a hard error) */
static int run_mdir_b(const char* img_name, const char* dir,
                      MDirEntry* out, int max)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mdir -b -i %s ::%s 2>/dev/null", img_name,
             dir);
    FILE* p = popen(cmd, "r");
    if (p == NULL)
        return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), p) != NULL) {
        char* s = strstr(line, "::/");
        if (s == NULL)
            continue;
        s += 3; /* past "::/" */
        char* nl = strchr(s, '\n');
        if (nl != NULL)
            *nl = '\0';
        /* the trailing '/' is the directory marker, not a separator:
         * drop it before splitting, then keep only the final path
         * component (lines repeat the requested dir prefix) */
        size_t len = strlen(s);
        int is_dir = len > 0 && s[len - 1] == '/';
        if (is_dir)
            s[len - 1] = '\0';
        const char* slash = strrchr(s, '/');
        const char* base = slash != NULL ? slash + 1 : s;
        size_t namelen = strlen(base);
        if (namelen == 0 || namelen >= sizeof(out[0].name))
            continue;
        if (n < max) {
            memcpy(out[n].name, base, namelen);
            out[n].name[namelen] = '\0';
            out[n].is_dir = is_dir;
            n++;
        }
    }
    int status = pclose(p);
    return status == 0 ? n : -1;
}

typedef struct {
    MDirEntry e[MDIR_MAX];
    int n;
} OurListing;

static void list_ours_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    OurListing* ours = user_data;

    (void)raw32;
    if (entry->attributes & 0x08) /* volume label: mdir -b omits it */
        return;
    if (entry->name[0] == '.')    /* dot entries: mdir -b omits them */
        return;
    if (ours->n < MDIR_MAX) {
        snprintf(ours->e[ours->n].name, sizeof(ours->e[0].name), "%s",
                 entry->name);
        ours->e[ours->n].is_dir = (entry->attributes & 0x10) != 0;
        ours->n++;
    }
}

static void assert_listing_matches_mdir(fat_ctx_t* ctx, const char* img_name,
                                        const char* dir, uint32_t cluster)
{
    MDirEntry theirs[MDIR_MAX];
    int tn = run_mdir_b(img_name, dir, theirs, MDIR_MAX);
    if (tn < 0)
        fprintf(stderr, "mdir failed: %s ::%s\n", img_name, dir);
    assert(tn >= 0);

    OurListing ours;
    memset(&ours, 0, sizeof(ours));
    assert(fat_iter_dir(ctx, cluster, list_ours_cb, &ours) == FAT_OK);

    if (ours.n != tn)
        fprintf(stderr, "listing size mismatch for ::%s: ours %d mdir %d\n",
                dir, ours.n, tn);
    assert(ours.n == tn);
    /* set comparison: every mdir entry matched by exactly one of ours
     * (with equal counts that is a bijection) */
    for (int i = 0; i < tn; i++) {
        int found = 0;
        for (int j = 0; j < ours.n; j++) {
            if (upper_eq(theirs[i].name, ours.e[j].name)) {
                if (!found && (theirs[i].is_dir != ours.e[j].is_dir))
                    fprintf(stderr, "dir-ness mismatch: %s%s vs %s\n",
                            theirs[i].name, theirs[i].is_dir ? "/" : "",
                            ours.e[j].name);
                assert(!found); /* names are unique: at most one match */
                assert(theirs[i].is_dir == ours.e[j].is_dir);
                found = 1;
            }
        }
        if (!found)
            fprintf(stderr, "mdir entry not in our listing: %s%s\n",
                    theirs[i].name, theirs[i].is_dir ? "/" : "");
        assert(found);
    }
}

/* byte-compare `mtype -i img ::path` with fat_read_file */
static void assert_mtype_matches_read(fat_ctx_t* ctx, const char* img_name,
                                      const char* path)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mtype -i %s ::%s 2>/dev/null", img_name,
             path);
    FILE* p = popen(cmd, "r");
    if (p == NULL)
        perror("mtype");
    assert(p != NULL);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de) == FAT_OK);
    uint8_t* want = NULL;
    size_t want_size = 0;
    assert(fat_read_file(ctx, &de, &want, &want_size) == FAT_OK);

    size_t got_total = 0;
    int mismatch = 0;
    int ch;
    while ((ch = fgetc(p)) != EOF) {
        if (got_total >= want_size || (uint8_t)ch != want[got_total]) {
            mismatch = 1;
            break;
        }
        got_total++;
    }
    int status = pclose(p);
    if (status != 0 || mismatch || got_total != want_size)
        fprintf(stderr, "mtype mismatch on %s: status %d, %zu of %zu bytes\n",
                path, status, got_total, want_size);
    assert(status == 0);
    assert(!mismatch);
    assert(got_total == want_size);
    free(want);
}

static void test_mtools_diff(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_listing_matches_mdir(ctx, IMG_NAME, "", FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert_listing_matches_mdir(ctx, IMG_NAME, "dir1", de.first_cluster);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de) == FAT_OK);
    assert_listing_matches_mdir(ctx, IMG_NAME, "dir2", de.first_cluster);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &de) == FAT_OK);
    assert_listing_matches_mdir(ctx, IMG_NAME, "dir2/subdir1",
                                de.first_cluster);

    assert_mtype_matches_read(ctx, IMG_NAME, "test_5kb.txt");

    fat_close(ctx);
}

static void test_fat16_mtools_diff(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_listing_matches_mdir(ctx, IMG16_NAME, "", FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert_listing_matches_mdir(ctx, IMG16_NAME, "dir1", de.first_cluster);

    assert_mtype_matches_read(ctx, IMG16_NAME, "test_5kb.txt");

    fat_close(ctx);
}

static void test_fat32_mtools_diff(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_listing_matches_mdir(ctx, IMG32_NAME, "", FAT_CLUSTER_ROOT);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert_listing_matches_mdir(ctx, IMG32_NAME, "dir1", de.first_cluster);

    assert_mtype_matches_read(ctx, IMG32_NAME, "test_5kb.txt");

    fat_close(ctx);
}

/* ------------------------------------------------------------------ */
/* shared read helpers for the FAT16/FAT32 suites                      */
/* ------------------------------------------------------------------ */

static void assert_read_string(fat_ctx_t* ctx, const char* path,
                               const char* want)
{
    static uint8_t sentinel; /* non-NULL start catches "OK but out unwritten" */
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    size_t want_size = strlen(want);

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de) == FAT_OK);
    assert(de.file_size == want_size);
    uint8_t* data = &sentinel;
    size_t size = 1;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_OK);
    assert(data != &sentinel);
    assert(size == want_size);
    assert(memcmp(data, want, want_size) == 0);
    free(data);
}

static void assert_read_matches_host(fat_ctx_t* ctx, const char* path,
                                     const char* host_file, size_t want_size)
{
    static uint8_t sentinel;
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de) == FAT_OK);
    assert(de.file_size == want_size);
    uint8_t* data = &sentinel;
    size_t size = 1;
    assert(fat_read_file(ctx, &de, &data, &size) == FAT_OK);
    assert(data != &sentinel);
    assert(size == want_size);
    {
        FILE* fp = fopen(host_file, "rb");
        if (fp == NULL)
            perror(host_file);
        assert(fp != NULL);
        uint8_t* expected = malloc(size);
        assert(expected != NULL);
        assert(fread(expected, 1, size, fp) == size);
        fclose(fp);
        assert(memcmp(data, expected, size) == 0);
        free(expected);
    }
    free(data);
}

/* ------------------------------------------------------------------ */
/* FAT16 fixture (demof16.fat, `make fat16`)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int labels, files, dirs;
    int saw_label, saw_hello, saw_5kb, saw_dir1;
} Root16Counts;

static void count_root16(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    Root16Counts* counts = user_data;

    (void)raw32;
    if (entry->attributes & 0x08) { /* volume label */
        counts->labels++;
        if (strcmp(entry->name, "DEMOF16") == 0)
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
        assert(entry->first_cluster == 13);
        assert(entry->attributes & 0x10);
    }
    if (entry->attributes & 0x10)
        counts->dirs++;
    else
        counts->files++;
}

static void test_fat16_open(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);

    assert(fat_get_type(ctx) == FT_FAT16);

    /* fatStart=1, rootDir=1+2*127=255, rootDirSectors=512*32/512=32,
     * dataStart=255+32=287, clusterCount=(32768-287)/1=32481 */
    const fat_geometry_t* g = fat_geometry(ctx);
    assert(g != NULL);
    assert(g->bytes_per_sector == 512);
    assert(g->sectors_per_cluster == 1);
    assert(g->reserved_sectors == 1);
    assert(g->fat_count == 2);
    assert(g->fat_sectors == 127);
    assert(g->root_entries == 512);
    assert(g->total_sectors == 32768);
    assert(g->fat_start_sector == 1);
    assert(g->root_dir_sector == 255);
    assert(g->root_dir_sectors == 32);
    assert(g->root_cluster == 0); /* FAT12/16: fixed root region */
    assert(g->data_start_sector == 287);
    assert(g->cluster_count == 32481);
    assert(g->cluster_count >= 4085 && g->cluster_count < 65525);
    assert(g->fat_sectors > 0);

    assert(fat_cluster_size(ctx) == 512u);

    /* FSInfo is FAT32-only */
    fat_fsinfo_t fi;
    assert(fat_fsinfo(ctx, &fi) == FAT_ERR_UNSUPPORTED);

    fat_close(ctx);
}

static void test_fat16_fat_entries(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);

    /* media/reserved entries and the verified chains */
    assert(fat_at(ctx, 0) == 0xFFF8); /* media 0xF8 dirty pattern */
    assert(fat_at(ctx, 1) == 0xFFFF);
    assert(fat_at(ctx, 2) == 0xFFFF); /* hello.txt: single cluster, EOC */
    assert(fat_at(ctx, 3) == 4);     /* test_5kb.txt chain 3->4->..->12 */
    assert(fat_at(ctx, 11) == 12);
    assert(fat_at(ctx, 12) == 0xFFFF); /* EOC at chain end, 0xFFFF >= 0xFFF8 */
    assert(fat_at(ctx, 13) == 0xFFFF); /* dir1 */
    assert(fat_at(ctx, 14) == 0xFFFF); /* sub1 */
    assert(fat_at(ctx, 15) == 0xFFFF); /* page.txt */
    assert(fat_at(ctx, 16) == 0xFFFF); /* hoge.txt */

    /* indices 0..cluster_count+1 are readable, beyond that is an error */
    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t v = 0;
    assert(fat_get_fat_entry(ctx, g->cluster_count + 1, &v) == FAT_OK);
    assert(fat_get_fat_entry(ctx, g->cluster_count + 2, &v) ==
           FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_fat16_root_iterate(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    Root16Counts counts = {0, 0, 0, 0, 0, 0, 0};

    assert(fat_iter_dir(ctx, FAT_CLUSTER_ROOT, count_root16, &counts) ==
           FAT_OK);
    /* DEMOF16 + hello.txt + test_5kb.txt + dir1 */
    assert(counts.labels == 1);
    assert(counts.files == 2);
    assert(counts.dirs == 1);
    assert(counts.saw_label && counts.saw_hello && counts.saw_5kb);
    assert(counts.saw_dir1);

    fat_close(ctx);
}

static void test_fat16_lookup_read(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert(de.first_cluster == 13);
    assert(de.attributes & 0x10);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de) == FAT_OK);
    assert(de.first_cluster == 14);

    /* multi-cluster chain: 4962 bytes over clusters 3->4->..->12 */
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert_read_string(ctx, "dir1/hoge.txt", "I am hoge.\n");
    assert_read_string(ctx, "dir1/sub1/page.txt", "You are page.\n");

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/noexist", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "noexist/foo", &de) ==
           FAT_ERR_PATH_NOT_FOUND);

    /* A2: synthetic root dirent on FAT16 too */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "..", &de) == FAT_OK);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(de.attributes & 0x10);

    fat_close(ctx);
}

/* A3: the FAT16 volume label never matches a lookup */
static void test_fat16_label_lookup(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "demof16", &de) ==
           FAT_ERR_NOT_FOUND);

    fat_close(ctx);
}

/* ------------------------------------------------------------------ */
/* FAT32 fixture (demof32.fat, `make fat32`)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int labels, files, dirs;
    int fillers;  /* F00.TXT..F39.TXT, in root-cluster order */
    int bad_name; /* non-filler unexpected entry */
    int saw_label, saw_hello, saw_5kb, saw_dir1;
} Root32Counts;

static void count_root32(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    Root32Counts* counts = user_data;

    (void)raw32;
    if (entry->attributes & 0x08) { /* volume label */
        counts->labels++;
        if (strcmp(entry->name, "DEMOF32") == 0)
            counts->saw_label = 1;
        return;
    }
    if (strcmp(entry->name, "HELLO.TXT") == 0) {
        counts->saw_hello = 1;
        assert(entry->first_cluster == 3);
        assert(entry->file_size == 12);
    } else if (strcmp(entry->name, "TEST_5KB.TXT") == 0) {
        counts->saw_5kb = 1;
        assert(entry->first_cluster == 4);
        assert(entry->file_size == 4962);
    } else if (strcmp(entry->name, "DIR1") == 0) {
        counts->saw_dir1 = 1;
        assert(entry->first_cluster == 56);
        assert(entry->attributes & 0x10);
    } else if (entry->name[0] == 'F' && strlen(entry->name) == 7) {
        /* FNN.TXT filler; mtools allocated them in copy order, so the
         * Nth filler sits at cluster 14+N regardless of which root
         * cluster of the 2->54->55 chain its dirent landed in */
        assert(entry->name[1] >= '0' && entry->name[1] <= '9');
        assert(entry->name[2] >= '0' && entry->name[2] <= '9');
        int n = (entry->name[1] - '0') * 10 + (entry->name[2] - '0');
        assert(n <= 39);
        assert(strcmp(entry->name + 3, ".TXT") == 0);
        assert(entry->first_cluster == 14u + (uint32_t)n);
        assert(entry->file_size == (n < 10 ? 14u : 15u));
        counts->fillers++;
    } else {
        counts->bad_name = 1;
    }
    if (entry->attributes & 0x10)
        counts->dirs++;
    else
        counts->files++;
}

static void test_fat32_open(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);

    assert(fat_get_type(ctx) == FT_FAT32);

    /* fatStart=32, dataStart=32+2*520=1072, rootDir region absent,
     * clusterCount=(67584-1072)/1=66512 >= 65525 */
    const fat_geometry_t* g = fat_geometry(ctx);
    assert(g != NULL);
    assert(g->bytes_per_sector == 512);
    assert(g->sectors_per_cluster == 1);
    assert(g->reserved_sectors == 32);
    assert(g->fat_count == 2);
    assert(g->fat_sectors == 520); /* tableSize32 for FAT32 */
    assert(g->root_entries == 0);
    assert(g->total_sectors == 67584); /* totalSectors32 */
    assert(g->fat_start_sector == 32);
    assert(g->root_dir_sector == 0);
    assert(g->root_dir_sectors == 0);
    assert(g->root_cluster == 2); /* FAT32: root is a cluster chain */
    assert(g->data_start_sector == 1072);
    assert(g->cluster_count == 66512);
    assert(g->cluster_count >= 65525);

    assert(fat_cluster_size(ctx) == 512u);

    fat_close(ctx);
}

static void test_fat32_fat_entries(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);

    /* masked media/reserved entries and the verified chains */
    assert(fat_at(ctx, 0) == 0x0FFFFFF8); /* media 0xF8 pattern, masked */
    assert(fat_at(ctx, 1) == 0x0FFFFFFF); /* raw 0xFFFFFFFF masked */
    assert(fat_at(ctx, 2) == 54);         /* root chain 2->54->55 */
    assert(fat_at(ctx, 3) == 0x0FFFFFFF); /* hello.txt: single cluster */
    assert(fat_at(ctx, 4) == 5);          /* test_5kb chain 4->5->..->13 */
    assert(fat_at(ctx, 12) == 13);
    assert(fat_at(ctx, 13) == 0x0FFFFFFF);
    assert(fat_at(ctx, 13) >= 0x0FFFFFF8u); /* EOC at chain end */
    assert(fat_at(ctx, 14) == 0x0FFFFFFF);  /* F00.TXT */
    assert(fat_at(ctx, 26) == 0x0FFFFFFF);  /* F12.TXT */
    assert(fat_at(ctx, 43) == 0x0FFFFFFF);  /* F29.TXT */
    assert(fat_at(ctx, 53) == 0x0FFFFFFF);  /* F39.TXT */
    assert(fat_at(ctx, 54) == 55);          /* second root cluster */
    assert(fat_at(ctx, 55) == 0x0FFFFFFF);  /* root chain ends here */
    assert(fat_at(ctx, 56) == 0x0FFFFFFF);  /* dir1 */
    assert(fat_at(ctx, 57) == 0x0FFFFFFF);  /* sub1 */
    assert(fat_at(ctx, 58) == 0x0FFFFFFF);  /* page.txt */
    assert(fat_at(ctx, 59) == 0x0FFFFFFF);  /* hoge.txt */

    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t v = 0;
    assert(fat_get_fat_entry(ctx, g->cluster_count + 1, &v) == FAT_OK);
    assert(fat_get_fat_entry(ctx, g->cluster_count + 2, &v) ==
           FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_fat32_root_iterate(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    Root32Counts counts = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    /* 44 entries spread over the 3-cluster root chain 2->54->55 */
    assert(fat_iter_dir(ctx, FAT_CLUSTER_ROOT, count_root32, &counts) ==
           FAT_OK);
    assert(counts.labels == 1);
    assert(counts.files == 42); /* hello + test_5kb + 40 fillers */
    assert(counts.dirs == 1);   /* dir1 */
    assert(counts.fillers == 40);
    assert(counts.bad_name == 0);
    assert(counts.saw_label && counts.saw_hello && counts.saw_5kb);
    assert(counts.saw_dir1);

    fat_close(ctx);
}

static void test_fat32_lookup_read(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert(de.first_cluster == 56);
    assert(de.attributes & 0x10);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de) == FAT_OK);
    assert(de.first_cluster == 57);

    /* multi-cluster chain: 4962 bytes over clusters 4->5->..->13 */
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert_read_string(ctx, "dir1/hoge.txt", "I am hoge.\n");
    assert_read_string(ctx, "dir1/sub1/page.txt", "You are page.\n");
    /* filler reached through the multi-cluster root: F29.TXT lives in the
     * third root cluster (55) */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "f29.txt", &de) == FAT_OK);
    assert(de.first_cluster == 43);
    assert(de.file_size == 15);

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/noexist", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "noexist/foo", &de) ==
           FAT_ERR_PATH_NOT_FOUND);

    /* A2: synthetic root dirent; the FAT32 root is a real cluster chain
     * (2->54->55), but "."/".." there are still synthesized, not read */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, ".", &de) == FAT_OK);
    assert(de.first_cluster == FAT_CLUSTER_ROOT);
    assert(de.attributes & 0x10);

    fat_close(ctx);
}

/* A3: the FAT32 volume label never matches a lookup */
static void test_fat32_label_lookup(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "demof32", &de) ==
           FAT_ERR_NOT_FOUND);

    fat_close(ctx);
}

static void test_fat32_fsinfo(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));

    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    /* verified against mdir at fixture-creation time: 34024448 bytes free
     * = 66454 clusters * 512B exactly (66512 total - 58 in use) */
    assert(fi.free_cluster_count == 66454);
    assert(fi.next_free_cluster == 59);
    assert(fi.free_cluster_count != 0xFFFFFFFFu);

    fat_close(ctx);
}

static void test_open_mem_errors_fat32(void)
{
    size_t size = 0;
    fat_ctx_t* ctx = NULL;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint64_t sum_before = checksum(orig, size);

    /* (a) rootCluster = 0: the FAT32 root has no chain head */
    static const uint8_t rc_zero[4] = {0x00, 0x00, 0x00, 0x00};
    uint8_t* img = mutated_copy(orig, size, BPB_ROOT_CLUSTER, rc_zero, 4);
    assert_open_mem_fails(img, size, FAT_ERR_INVALID_BPB);
    free(img);

    /* (b) rootCluster = cluster_count + 5 = 66517: outside the data region
     * (valid data clusters are 2..66513) */
    static const uint8_t rc_oob[4] = {0xD5, 0x03, 0x01, 0x00};
    img = mutated_copy(orig, size, BPB_ROOT_CLUSTER, rc_oob, 4);
    assert_open_mem_fails(img, size, FAT_ERR_INVALID_BPB);
    free(img);

    /* (c) FSInfo lead signature (offset 0 of the FSInfo sector, sector
     * number taken from BPB@48) zeroed: open still succeeds and fat_fsinfo
     * degrades to "unknown" instead of trusting the stale counters */
    img = copy_image(orig, size);
    unsigned fsinfo_sector =
        (unsigned)img[BPB_FSINFO_SECTOR] |
        ((unsigned)img[BPB_FSINFO_SECTOR + 1] << 8);
    assert(fsinfo_sector == 1);
    memset(img + (size_t)fsinfo_sector * 512u, 0, 4);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 0xFFFFFFFFu);
    assert(fi.next_free_cluster == 0xFFFFFFFFu);
    fat_close(ctx);
    ctx = NULL;

    /* (d) chain loop through a 32-bit FAT entry: TEST_5KB.TXT runs
     * 4->5->..->13; FAT[5] = 5 makes it point at itself, so the read must
     * trip the chain guard instead of spinning */
    img = copy_image(orig, size);
    set_fat32_entry(img, 5, 5);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    uint8_t* data = NULL;
    size_t n = 0;
    assert(fat_read_file(ctx, &de, &data, &n) == FAT_ERR_BAD_CLUSTER);
    assert(data == NULL); /* no buffer may escape on the error path */
    fat_close(ctx);
    ctx = NULL;

    /* fat_open_mem copies the image: the pristine buffer survives every
     * mutation round trip untouched */
    assert(checksum(orig, size) == sum_before);
    free(orig);
}

/* ------------------------------------------------------------------ */
/* phase 4: write support (fat_set_fat_entry, fat_alloc_cluster,       */
/* fat_free_chain, fat_add_dirent, fat_write_file, fat_write)          */
/* ------------------------------------------------------------------ */

/* Decode a 12-bit FAT entry from a raw image, FAT table at `base_sector`
 * (the packing shares a byte between even/odd neighbours). */
static uint16_t raw_fat12_at(const uint8_t* img, unsigned base_sector,
                             uint32_t cluster)
{
    size_t off = (size_t)base_sector * 512u + cluster * 3u / 2u;
    if (cluster % 2u == 0u)
        return (uint16_t)(img[off] | ((img[off + 1] & 0x0Fu) << 8));
    return (uint16_t)((img[off] >> 4) | ((uint16_t)img[off + 1] << 4));
}

static void count_any_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    (void)entry;
    (void)raw32;
    (*(int*)user_data)++;
}

/* live entry count of a directory (label and dot entries included) */
static int count_dir_entries(fat_ctx_t* ctx, uint32_t cluster)
{
    int n = 0;
    assert(fat_iter_dir(ctx, cluster, count_any_cb, &n) == FAT_OK);
    return n;
}

/* data clusters in the chain headed at `first` (stops at EOC/broken) */
static uint32_t chain_length(const fat_ctx_t* ctx, uint32_t first)
{
    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t n = 0;
    uint32_t c = first;
    while (c >= 2u && c <= g->cluster_count + 1u && n < 10000u) {
        n++;
        c = fat_at(ctx, c);
    }
    return n;
}

/* free data clusters, by the FAT itself (ground truth for FSInfo checks) */
static uint32_t count_free_clusters(const fat_ctx_t* ctx)
{
    uint32_t n = 0;
    for (uint32_t c = 2u; c <= fat_geometry(ctx)->cluster_count + 1u; c++)
        if (fat_at(ctx, c) == 0)
            n++;
    return n;
}

/* deterministic write pattern */
static uint8_t* make_pattern(size_t n)
{
    uint8_t* p = malloc(n);
    assert(p != NULL);
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 7u + 3u);
    return p;
}

/* After writing `name` into `dir_cluster`: lookup finds it, the dirent
 * size/first_cluster are exact, the FAT chain holds `want_clusters` data
 * clusters, and both fat_read_file and a fat_file_t cursor deliver the
 * original bytes. `dir_cluster` may be a directory cluster or a start
 * cluster plus a path (lookup resolves both). */
static void assert_write_roundtrip(fat_ctx_t* ctx, uint32_t dir_cluster,
                                   const char* name, const uint8_t* data,
                                   size_t size, uint32_t want_clusters)
{
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, dir_cluster, name, &de) == FAT_OK);
    assert(de.file_size == size);
    if (size == 0) {
        assert(de.first_cluster == 0); /* empty file: no chain at all */
        return;
    }
    assert(de.first_cluster >= 2u);
    assert(de.first_cluster <= fat_geometry(ctx)->cluster_count + 1u);
    assert(chain_length(ctx, de.first_cluster) == want_clusters);

    uint8_t* back = NULL;
    size_t back_size = 0;
    assert(fat_read_file(ctx, &de, &back, &back_size) == FAT_OK);
    assert(back_size == size);
    assert(memcmp(back, data, size) == 0);
    free(back);

    fat_file_t* f = NULL;
    assert(fat_file_open(ctx, &de, &f) == FAT_OK);
    uint8_t* buf = malloc(size);
    assert(buf != NULL);
    size_t got = 0;
    assert(fat_file_read(f, buf, size, &got) == FAT_OK);
    assert(got == size);
    assert(memcmp(buf, data, size) == 0);
    fat_file_close(f);
    free(buf);
}

static void assert_same_geometry(const fat_geometry_t* a,
                                 const fat_geometry_t* b)
{
    assert(a->bytes_per_sector == b->bytes_per_sector);
    assert(a->sectors_per_cluster == b->sectors_per_cluster);
    assert(a->reserved_sectors == b->reserved_sectors);
    assert(a->fat_count == b->fat_count);
    assert(a->fat_sectors == b->fat_sectors);
    assert(a->root_entries == b->root_entries);
    assert(a->total_sectors == b->total_sectors);
    assert(a->fat_start_sector == b->fat_start_sector);
    assert(a->root_dir_sector == b->root_dir_sector);
    assert(a->root_dir_sectors == b->root_dir_sectors);
    assert(a->root_cluster == b->root_cluster);
    assert(a->data_start_sector == b->data_start_sector);
    assert(a->cluster_count == b->cluster_count);
}

/* 12.1: FAT12 nibble neighbours survive the read-modify-write, both FAT
 * copies receive the update, and the argument/value ranges hold. */
static void test_set_fat_entry12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    /* 3 (even) and 4 (odd) sit inside the test_5kb chain; 9/10 are a free
     * even/odd pair.  The written entries change, every neighbour keeps
     * its original relationship. */
    assert(fat_set_fat_entry(ctx, 3, 0x123) == FAT_OK);
    assert(fat_set_fat_entry(ctx, 4, 0x456) == FAT_OK);
    assert(fat_set_fat_entry(ctx, 9, 0x0AA) == FAT_OK);
    assert(fat_set_fat_entry(ctx, 10, 0x0BB) == FAT_OK);
    assert(fat_at(ctx, 2) == 0xFFF);
    assert(fat_at(ctx, 3) == 0x123);
    assert(fat_at(ctx, 4) == 0x456);
    assert(fat_at(ctx, 5) == 6);
    assert(fat_at(ctx, 6) == 7);
    assert(fat_at(ctx, 7) == 0xFFF);
    assert(fat_at(ctx, 8) == 0xFFF);
    assert(fat_at(ctx, 9) == 0x0AA);
    assert(fat_at(ctx, 10) == 0x0BB);
    assert(fat_at(ctx, 11) == 43);

    /* FREE and EOC are ordinary writable values */
    assert(fat_set_fat_entry(ctx, 9, 0) == FAT_OK);
    assert(fat_at(ctx, 9) == 0);
    assert(fat_set_fat_entry(ctx, 9, 0xFFF) == FAT_OK);
    assert(fat_at(ctx, 9) == 0xFFF);

    /* cluster range: 2..cluster_count+1 */
    assert(fat_set_fat_entry(NULL, 3, 1) == FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(ctx, 0, 1) == FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(ctx, 1, 1) == FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(ctx, g->cluster_count + 1, 1) == FAT_OK);
    assert(fat_set_fat_entry(ctx, g->cluster_count + 2, 1) ==
           FAT_ERR_INVALID_ARG);
    /* value range: 12-bit entries cap at 0xFFF */
    assert(fat_set_fat_entry(ctx, 3, 0x1000) == FAT_ERR_INVALID_ARG);

    /* both FAT copies: flush and compare the raw tables byte for byte
     * (they start identical, so mirrored writes keep them identical) and
     * decode the touched entries by hand in each table */
    assert(fat_write(ctx, "/tmp/p4_set12.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_set12.fat", &fn);
    assert(fn == size);
    assert(memcmp(back + FIXTURE_FAT_SECTOR * 512u, back + 4u * 512u,
                  3u * 512u) == 0);
    for (unsigned t = 0; t < 2u; t++) {
        unsigned base = t == 0u ? FIXTURE_FAT_SECTOR : 4u;
        assert(raw_fat12_at(back, base, 2) == 0xFFF);
        assert(raw_fat12_at(back, base, 3) == 0x123);
        assert(raw_fat12_at(back, base, 4) == 0x456);
        assert(raw_fat12_at(back, base, 5) == 6);
        assert(raw_fat12_at(back, base, 6) == 7);
        assert(raw_fat12_at(back, base, 7) == 0xFFF);
        assert(raw_fat12_at(back, base, 9) == 0xFFF);
        assert(raw_fat12_at(back, base, 10) == 0x0BB);
    }
    free(back);
    remove("/tmp/p4_set12.fat");

    fat_close(ctx);
    free(orig);
}

/* 12.2: alloc hands out a cluster that was FREE, marks it EOC (the exact
 * type constant), and a second alloc picks a different one.  A fully
 * allocated FAT is FAT_ERR_DISK_FULL. */
static void test_alloc_cluster12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint32_t c1 = 0;
    uint32_t c2 = 0;
    assert(fat_alloc_cluster(ctx, &c1) == FAT_OK);
    assert(c1 >= 2u && c1 <= fat_geometry(ctx)->cluster_count + 1u);
    assert(raw_fat12_at(orig, FIXTURE_FAT_SECTOR, c1) == 0); /* was free */
    assert(fat_at(ctx, c1) == 0xFFF); /* now EOC, exact constant */
    assert(fat_alloc_cluster(ctx, &c2) == FAT_OK);
    assert(c2 != c1);
    assert(raw_fat12_at(orig, FIXTURE_FAT_SECTOR, c2) == 0);
    assert(fat_at(ctx, c2) == 0xFFF);
    assert(fat_at(ctx, c1) == 0xFFF); /* the second alloc changed nothing */

    assert(fat_alloc_cluster(NULL, &c1) == FAT_ERR_INVALID_ARG);
    assert(fat_alloc_cluster(ctx, NULL) == FAT_ERR_INVALID_ARG);
    fat_close(ctx);

    /* DISK_FULL: every data entry nonzero leaves nothing to allocate */
    img = copy_image(orig, size);
    for (uint32_t c = 2u; c <= 713u + 1u; c++)
        if (raw_fat12_at(img, FIXTURE_FAT_SECTOR, c) == 0)
            set_fat12_entry(img, c, 0x001);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    assert(fat_alloc_cluster(ctx, &c1) == FAT_ERR_DISK_FULL);
    fat_close(ctx);
    free(orig);
}

/* 12.3: freeing the known chain 3->4->5->6->7 zeroes every entry (and the
 * first alloc afterwards can take cluster 3 back); a looping chain frees
 * what it walked and reports FAT_ERR_BAD_CLUSTER. */
static void test_free_chain12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    assert(fat_free_chain(ctx, 3) == FAT_OK);
    for (uint32_t c = 3u; c <= 7u; c++)
        assert(fat_at(ctx, c) == 0);
    /* the scan restarts at the lowest cluster, so 3 comes back first */
    uint32_t c = 0;
    assert(fat_alloc_cluster(ctx, &c) == FAT_OK);
    assert(c == 3);

    /* single-cluster chain */
    assert(fat_free_chain(ctx, 2) == FAT_OK);
    assert(fat_at(ctx, 2) == 0);
    fat_close(ctx);

    /* looping chain: FAT[8] = 8 */
    img = copy_image(orig, size);
    set_fat12_entry(img, 8, 8);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    assert(fat_free_chain(ctx, 8) == FAT_ERR_BAD_CLUSTER);
    assert(fat_at(ctx, 8) == 0); /* the walked entry was still freed */

    assert(fat_free_chain(NULL, 3) == FAT_ERR_INVALID_ARG);
    assert(fat_free_chain(ctx, 0) == FAT_ERR_INVALID_ARG);
    assert(fat_free_chain(ctx, 1) == FAT_ERR_INVALID_ARG);
    assert(fat_free_chain(ctx, fat_geometry(ctx)->cluster_count + 2) ==
           FAT_ERR_INVALID_ARG);
    fat_close(ctx);
    free(orig);
}

/* 12.4: a plain create in dir1 fills the entry from tmpl (name included
 * from `name`, not tmpl.name) and our own lookup finds every field. */
static void test_add_dirent_create(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    strcpy(tmpl.name, "IGNORED"); /* on-disk name comes from `name` */
    tmpl.attributes = 0x20;
    tmpl.creation_time_tenth = 78;
    tmpl.creation_time = dos_time_bits(12, 34, 56);
    tmpl.creation_date = dos_date_bits(2026, 10, 5);
    tmpl.last_access_date = dos_date_bits(2026, 10, 6);
    tmpl.last_write_time = dos_time_bits(21, 5, 4);
    tmpl.last_write_date = dos_date_bits(2025, 12, 31);
    tmpl.first_cluster = 99;
    tmpl.file_size = 123;

    assert(fat_add_dirent(ctx, 8, "newfile.txt", &tmpl) == FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/newfile.txt", &de) ==
           FAT_OK);
    assert(strcmp(de.name, "NEWFILE.TXT") == 0);
    assert(de.attributes == 0x20);
    assert(de.creation_time_tenth == 78);
    assert(de.creation_time == dos_time_bits(12, 34, 56));
    assert(de.creation_date == dos_date_bits(2026, 10, 5));
    assert(de.last_access_date == dos_date_bits(2026, 10, 6));
    assert(de.last_write_time == dos_time_bits(21, 5, 4));
    assert(de.last_write_date == dos_date_bits(2025, 12, 31));
    assert(de.first_cluster == 99);
    assert(de.file_size == 123);

    /* timestamps decode through the public helper */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    assert(fat_dos_date_to_tm(de.creation_date, de.creation_time,
                              de.creation_time_tenth, &tm) == FAT_OK);
    assert_tm_fields(&tm, 126, 9, 5, 12, 34, 56);

    /* dir1: 5 live entries before, 6 after */
    assert(count_dir_entries(ctx, 8) == 6);

    fat_close(ctx);
    free(orig);
}

/* 12.4: the first deleted (0xE5) slot is reused -- HOGE.TXT's slot in
 * dir1, raw-verified at the exact byte offset. */
static void test_add_dirent_reuse(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);

    /* dir1 = cluster 8, data byte offset (14 + 6*2) * 512; mark the
     * HOGE.TXT entry deleted (first byte 0xE5) */
    size_t dir1_off = 26u * 512u;
    size_t hoge_off = 0;
    for (unsigned i = 0; i < 32u; i++) {
        if (memcmp(img + dir1_off + i * 32u, "HOGE    TXT", 11) == 0) {
            hoge_off = dir1_off + i * 32u;
            break;
        }
    }
    assert(hoge_off != 0);
    img[hoge_off] = 0xE5;

    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    tmpl.first_cluster = 7;
    tmpl.file_size = 5;
    assert(fat_add_dirent(ctx, 8, "hoga.txt", &tmpl) == FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoga.txt", &de) == FAT_OK);
    assert(de.first_cluster == 7);
    assert(de.file_size == 5);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de) ==
           FAT_ERR_NOT_FOUND);

    /* raw slot check on the flushed image: HOGA sits exactly where HOGE
     * was, exactly once, and HOGE is gone */
    assert(fat_write(ctx, "/tmp/p4_reuse.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_reuse.fat", &fn);
    int hoga_count = 0;
    int hoge_count = 0;
    size_t hoga_off = 0;
    for (unsigned i = 0; i < 32u; i++) {
        if (memcmp(back + dir1_off + i * 32u, "HOGA    TXT", 11) == 0) {
            hoga_count++;
            hoga_off = dir1_off + i * 32u;
        }
        if (memcmp(back + dir1_off + i * 32u, "HOGE    TXT", 11) == 0)
            hoge_count++;
    }
    assert(hoga_count == 1);
    assert(hoga_off == hoge_off);
    assert(hoge_count == 0);
    free(back);
    remove("/tmp/p4_reuse.fat");

    /* hoga replaced hoge: still 5 live entries */
    assert(count_dir_entries(ctx, 8) == 5);

    fat_close(ctx);
    free(orig);
}

/* EXISTS / NAME_TOO_LONG / DIR_FULL / INVALID_ARG paths of fat_add_dirent */
static void test_add_dirent_errors(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    fat_dirent_t de;

    /* EXISTS: hello.txt is live, the original entry stays untouched */
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    assert(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "hello.txt", &tmpl) ==
           FAT_ERR_EXISTS);
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 12);
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5);
    fat_close(ctx);

    /* NAME_TOO_LONG and NULL arguments */
    img = copy_image(orig, size);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    assert(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "toolongname.txt", &tmpl) ==
           FAT_ERR_NAME_TOO_LONG);
    assert(fat_add_dirent(NULL, FAT_CLUSTER_ROOT, "x", &tmpl) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, NULL, &tmpl) ==
           FAT_ERR_INVALID_ARG);
    fat_close(ctx);

    /* DIR_FULL: rootEntryCount = 5 = exactly the live root entries
     * (label + hello + test_5kb + dir1 + dir2, slots 0..4), so the fixed
     * FAT12 root region has no free slot and cannot extend */
    img = copy_image(orig, size);
    static const uint8_t re5[2] = {0x05, 0x00};
    memcpy(img + BPB_ROOT_ENTRIES, re5, 2);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    assert(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "extra.txt", &tmpl) ==
           FAT_ERR_DIR_FULL);
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 12);
    fat_close(ctx);

    free(orig);
}

/* 12.7 + mtools oracle: a 5000-byte file over 5 clusters, byte-exact
 * through every read path, with mtype/mdir agreeing on the flushed image */
static void test_write_file_roundtrip12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    size_t n = 5000;
    uint8_t* data = make_pattern(n);

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    tmpl.creation_date = dos_date_bits(2026, 2, 3);
    tmpl.creation_time = dos_time_bits(9, 8, 6);
    tmpl.last_write_date = dos_date_bits(2026, 2, 3);
    tmpl.last_write_time = dos_time_bits(9, 8, 6);

    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          &tmpl) == FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n, 5);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "new.txt", &de) == FAT_OK);
    assert(de.attributes == 0x20);
    assert(de.creation_date == dos_date_bits(2026, 2, 3));
    assert(de.creation_time == dos_time_bits(9, 8, 6));
    assert(de.last_write_date == dos_date_bits(2026, 2, 3));
    assert(de.last_write_time == dos_time_bits(9, 8, 6));

    /* attributes arrive verbatim from tmpl (RO|archive; hidden/system
     * would vanish from mdir's default listing and break the oracle) */
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x21;
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "attr.txt",
                          (const uint8_t*)"xyz", 3, &tmpl) == FAT_OK);
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "attr.txt", &de) == FAT_OK);
    assert(de.attributes == 0x21);

    /* pre-existing content untouched */
    assert_read_string(ctx, "hello.txt", "hello world\n");

    /* mtools oracle on the flushed image */
    assert(fat_write(ctx, "/tmp/p4_rt12.fat") == FAT_OK);
    assert_mtype_matches_read(ctx, "/tmp/p4_rt12.fat", "new.txt");
    assert_listing_matches_mdir(ctx, "/tmp/p4_rt12.fat", "",
                                FAT_CLUSTER_ROOT);
    remove("/tmp/p4_rt12.fat");

    free(data);
    fat_close(ctx);
    free(orig);
}

/* cluster-count boundaries around 1024-byte clusters (k, k+-1) */
static void test_write_file_boundaries(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    static const size_t sizes[] = {1023, 1024, 1025, 2048, 2049};
    static const uint32_t clusters[] = {1, 1, 2, 2, 3};
    char name[16];
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "b%zu.txt", sizes[i]);
        uint8_t* data = make_pattern(sizes[i]);
        assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, name, data, sizes[i],
                              NULL) == FAT_OK);
        assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, name, data, sizes[i],
                               clusters[i]);
        free(data);
    }

    fat_close(ctx);
    free(orig);
}

/* empty file (no chain, first_cluster 0), tmpl NULL defaults (ATTR_ARCHIVE
 * + zero timestamps), and the EXISTS / INVALID_ARG / NAME_TOO_LONG paths */
static void test_write_file_small(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    /* 662 free data clusters: 713 total minus the 51 in use (every
     * cluster 2..52 holds hello/test_5kb/dir1/dir2 and the 33+2 empty
     * subdirectories spread under dir1 and dir2; FAT-verified) */
    uint32_t free_before = count_free_clusters(ctx);
    assert(free_before == 662u);

    /* empty file: data NULL is only legal with size 0 */
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "empty.txt", NULL, 0,
                          NULL) == FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "empty.txt", &de) == FAT_OK);
    assert(de.file_size == 0);
    assert(de.first_cluster == 0);
    /* NULL tmpl = ATTR_ARCHIVE and zero timestamps */
    assert(de.attributes == 0x20);
    assert(de.creation_time_tenth == 0);
    assert(de.creation_time == 0);
    assert(de.creation_date == 0);
    assert(de.last_access_date == 0);
    assert(de.last_write_time == 0);
    assert(de.last_write_date == 0);
    uint8_t* back = NULL;
    size_t bn = 0;
    assert(fat_read_file(ctx, &de, &back, &bn) == FAT_OK);
    assert(back == NULL);
    assert(bn == 0);

    /* empty wrote no cluster */
    assert(count_free_clusters(ctx) == free_before);

    /* EXISTS: the original survives byte for byte */
    uint8_t one = 'x';
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "hello.txt", &one, 1,
                          NULL) == FAT_ERR_EXISTS);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert(count_free_clusters(ctx) == free_before); /* nothing leaked */

    /* data NULL with size > 0 */
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "null.dat", NULL, 10,
                          NULL) == FAT_ERR_INVALID_ARG);
    /* NULL ctx / NULL name / unrepresentable name */
    assert(fat_write_file(NULL, FAT_CLUSTER_ROOT, "x", NULL, 0, NULL) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, NULL, NULL, 0, NULL) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "toolongname.txt", NULL, 0,
                          NULL) == FAT_ERR_NAME_TOO_LONG);

    fat_close(ctx);
    free(orig);
}

/* 12.7 rollback: DISK_FULL mid-chain leaves the FAT, the directory and
 * the pre-existing files exactly as they were */
static void test_write_file_rollback(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);

    /* fill every free entry except 300 and 301 with a nonzero marker */
    for (uint32_t c = 2u; c <= 713u + 1u; c++) {
        if (c == 300u || c == 301u)
            continue;
        if (raw_fat12_at(img, FIXTURE_FAT_SECTOR, c) == 0)
            set_fat12_entry(img, c, 0x001);
    }
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    size_t n = 5000; /* needs 5 clusters, only 2 are free */
    uint8_t* data = make_pattern(n);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "big.txt", data, n,
                          NULL) == FAT_ERR_DISK_FULL);
    free(data);

    /* the two clusters were rolled back to FREE, nothing else leaked */
    assert(fat_at(ctx, 300) == 0);
    assert(fat_at(ctx, 301) == 0);
    assert(count_free_clusters(ctx) == 2u);
    /* no dirent appeared, the originals are intact */
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "big.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);

    fat_close(ctx);
    free(orig);
}

/* write into a subdirectory resolved through lookup */
static void test_write_file_subdir(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t dir;
    memset(&dir, 0, sizeof(dir));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &dir) == FAT_OK);
    assert(dir.first_cluster == 12);

    size_t n = 100;
    uint8_t* data = make_pattern(n);
    assert(fat_write_file(ctx, dir.first_cluster, "new.txt", data, n,
                          NULL) == FAT_OK);
    /* reachable through the full path; 100 bytes fit one 1024B cluster */
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/new.txt",
                           data, n, 1);
    /* subdir1 had ".", "..", page.txt, test_5kb.txt; now 5 entries */
    assert(count_dir_entries(ctx, dir.first_cluster) == 5);

    free(data);
    fat_close(ctx);
    free(orig);
}

/* fat_write: flush to an explicit path, reopen with fat_open, everything
 * survived; NULL handling and an unwritable destination */
static void test_write_flush(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    size_t n = 5000;
    uint8_t* data = make_pattern(n);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);

    assert(fat_write(NULL, "/tmp/p4_flush12.fat") == FAT_ERR_INVALID_ARG);
    assert(fat_write(ctx, NULL) == FAT_ERR_INVALID_ARG);

    assert(fat_write(ctx, "/tmp/p4_flush12.fat") == FAT_OK);

    /* exactly the whole image, and it reopens clean */
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_flush12.fat", &fn);
    assert(fn == size);
    free(back);
    fat_ctx_t* fresh = NULL;
    assert(fat_open("/tmp/p4_flush12.fat", &fresh) == FAT_OK);
    assert(fat_get_type(fresh) == FT_FAT12);
    assert_same_geometry(fat_geometry(ctx), fat_geometry(fresh));

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(fresh, FAT_CLUSTER_ROOT, "new.txt", &de) == FAT_OK);
    assert(de.file_size == n);
    uint8_t* rdata = NULL;
    size_t rn = 0;
    assert(fat_read_file(fresh, &de, &rdata, &rn) == FAT_OK);
    assert(rn == n);
    assert(memcmp(rdata, data, n) == 0);
    free(rdata);
    assert_read_string(fresh, "hello.txt", "hello world\n");
    fat_close(fresh);

    /* an unwritable destination reports FAT_ERR_IO */
    assert(fat_write(ctx, "/tmp/no_such_dir_p4/x.fat") == FAT_ERR_IO);

    remove("/tmp/p4_flush12.fat");
    free(data);
    fat_close(ctx);
    free(orig);
}

/* FAT16: entry value ceiling, alloc/free accounting, multi-cluster write */
static void test_fat16_write(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG16_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    uint32_t before = count_free_clusters(ctx);

    /* set/get roundtrip and the 16-bit value ceiling */
    assert(fat_at(ctx, 20) == 0); /* free cluster past the fixture chains */
    assert(fat_set_fat_entry(ctx, 20, 0x1234) == FAT_OK);
    assert(fat_at(ctx, 20) == 0x1234);
    assert(fat_set_fat_entry(ctx, 21, 0xFFFF) == FAT_OK); /* EOC */
    assert(fat_at(ctx, 21) == 0xFFFF);
    assert(fat_set_fat_entry(ctx, 22, 0x10000) == FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(ctx, 1, 1) == FAT_ERR_INVALID_ARG);

    /* alloc then free restores the free count exactly */
    uint32_t c = 0;
    assert(fat_alloc_cluster(ctx, &c) == FAT_OK);
    assert(fat_at(ctx, c) == 0xFFFF);
    assert(fat_set_fat_entry(ctx, 20, 0) == FAT_OK); /* restore */
    assert(fat_set_fat_entry(ctx, 21, 0) == FAT_OK);
    assert(fat_free_chain(ctx, c) == FAT_OK);
    assert(count_free_clusters(ctx) == before);

    /* 1000 bytes over 2 clusters of 512B */
    size_t n = 1000;
    uint8_t* data = make_pattern(n);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n, 2);
    free(data);

    fat_close(ctx);
    free(orig);
}

/* FAT32: the upper 4 reserved bits of an entry survive set_fat_entry,
 * both mirrored tables included */
static void test_set_fat_entry32(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    set_fat32_entry(img, 10, 0xA000000Au); /* reserved nibble 0xA, val 10 */
    /* keep the mirror (FAT #1, 520 sectors later) in agreement so the
     * image stays self-consistent */
    memcpy(img + (size_t)(F32_FAT_SECTOR + 520u) * 512u + 10u * 4u,
           img + (size_t)F32_FAT_SECTOR * 512u + 10u * 4u, 4);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    uint32_t before9 = fat_at(ctx, 9);
    uint32_t before11 = fat_at(ctx, 11);
    assert(fat_set_fat_entry(ctx, 10, 0x01234567u) == FAT_OK);
    assert(fat_at(ctx, 10) == 0x01234567u); /* masked readback */
    assert(fat_at(ctx, 9) == before9);
    assert(fat_at(ctx, 11) == before11);

    assert(fat_set_fat_entry(ctx, 10, 0x10000000u) == FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(ctx, 0, 1) == FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(ctx, g->cluster_count + 2, 1) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_set_fat_entry(NULL, 10, 1) == FAT_ERR_INVALID_ARG);

    /* raw check: reserved nibble kept in both FAT copies after flush */
    assert(fat_write(ctx, "/tmp/p4_set32.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_set32.fat", &fn);
    assert(fn == size);
    for (unsigned t = 0; t < 2u; t++) {
        size_t off =
            (size_t)(F32_FAT_SECTOR + t * 520u) * 512u + 10u * 4u;
        uint32_t v = (uint32_t)back[off] |
                     ((uint32_t)back[off + 1] << 8) |
                     ((uint32_t)back[off + 2] << 16) |
                     ((uint32_t)back[off + 3] << 24);
        assert(v == 0xA1234567u); /* 0xA<<28 | 0x01234567 */
    }
    free(back);
    remove("/tmp/p4_set32.fat");

    fat_close(ctx);
    free(orig);
}

/* FAT32: FSInfo free count steps down per alloc and back up per free,
 * and the next-free hint stays a sane cluster number */
static void test_alloc_free32(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    uint32_t free_before = fi.free_cluster_count;
    assert(free_before == 66454u); /* fixture fact, mdir cross-checked */

    uint32_t c = 0;
    assert(fat_alloc_cluster(ctx, &c) == FAT_OK);
    assert(fat_at(ctx, c) == 0x0FFFFFFFu); /* EOC, exact constant */
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == free_before - 1u);
    assert(fi.next_free_cluster >= 2u);
    assert(fi.next_free_cluster <= g->cluster_count + 1u);
    assert(fi.next_free_cluster != 0xFFFFFFFFu);

    /* freeing it back restores the count (hint head update is sane) */
    assert(fat_free_chain(ctx, c) == FAT_OK);
    assert(fat_at(ctx, c) == 0);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == free_before);
    assert(fi.next_free_cluster >= 2u);
    assert(fi.next_free_cluster <= g->cluster_count + 1u);

    fat_close(ctx);
    free(orig);
}

/* FAT32 root chain extension: clusters 2->54->55 hold 44 entries; cluster
 * 55 has 4 free slots, so the 5th add extends the chain by one zeroed
 * cluster (one allocation in total -- dirent-only adds take no data). */
static void test_fat32_root_extend(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 44);
    assert(fat_at(ctx, 55) == 0x0FFFFFFFu); /* chain ends at 55 */

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    for (int i = 1; i <= 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "x%d.txt", i);
        assert(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, name, &tmpl) == FAT_OK);
    }

    /* the chain grew: 55 now links to a fresh EOC cluster */
    uint32_t next = fat_at(ctx, 55);
    assert(next >= 2u && next <= g->cluster_count + 1u);
    assert(fat_at(ctx, next) == 0x0FFFFFFFu);

    /* 44 + 5 live entries */
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 49);

    /* dirent-only adds allocate exactly the one extension cluster */
    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u - 1u);

    /* the new cluster is zeroed except for the single fresh entry (x5,
     * the 5th add, is the one that forced the extension) */
    assert(fat_write(ctx, "/tmp/p4_ext32.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_ext32.fat", &fn);
    size_t cl_bytes = (size_t)g->sectors_per_cluster * g->bytes_per_sector;
    size_t cl_off = ((size_t)g->data_start_sector + (next - 2u) *
                    g->sectors_per_cluster) * g->bytes_per_sector;
    assert(memcmp(back + cl_off, "X5      TXT", 11) == 0);
    for (size_t i = 32; i < cl_bytes; i++)
        assert(back[cl_off + i] == 0);
    free(back);
    remove("/tmp/p4_ext32.fat");

    fat_close(ctx);
    free(orig);
}

/* FAT32 write_file: FSInfo steps down by the data cluster count, subdirs
 * work, and the mtools oracle agrees on the flushed image */
static void test_fat32_write_file(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);

    /* 5000 bytes over 10 clusters of 512B */
    size_t n = 5000;
    uint8_t* data = make_pattern(n);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n, 10);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u - 10u);
    assert(fi.next_free_cluster >= 2u);
    assert(fi.next_free_cluster <= g->cluster_count + 1u);

    /* empty file allocates nothing */
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "empty.txt", NULL, 0,
                          NULL) == FAT_OK);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u - 10u);

    /* write into dir1 (cluster 56), one more cluster */
    fat_dirent_t dir;
    memset(&dir, 0, sizeof(dir));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &dir) == FAT_OK);
    size_t sn = 100;
    uint8_t* sdata = make_pattern(sn);
    assert(fat_write_file(ctx, dir.first_cluster, "sub.txt", sdata, sn,
                          NULL) == FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "dir1/sub.txt", sdata, sn,
                           1);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u - 11u);

    /* originals intact + mtools oracle on the flushed image */
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert(fat_write(ctx, "/tmp/p4_rt32.fat") == FAT_OK);
    assert_mtype_matches_read(ctx, "/tmp/p4_rt32.fat", "new.txt");
    assert_listing_matches_mdir(ctx, "/tmp/p4_rt32.fat", "",
                                FAT_CLUSTER_ROOT);
    assert_listing_matches_mdir(ctx, "/tmp/p4_rt32.fat", "dir1",
                                dir.first_cluster);
    remove("/tmp/p4_rt32.fat");

    free(sdata);
    free(data);
    fat_close(ctx);
    free(orig);
}

/* ------------------------------------------------------------------ */
/* phase 5: I/O abstraction (fat_io_t backends, fat_open_io,           */
/* fat_sync, write-back visibility)                                    */
/* ------------------------------------------------------------------ */

/* fixture byte boundaries (demof12, see test_open): the FAT tables
 * occupy sectors 1..6 ([512, 3584)), the fixed root region sectors
 * 7..13 ([3584, 7168)), the data area starts at sector 14 */
#define F12_ROOT_BYTES (FIXTURE_ROOT_SECTOR * 512u)
#define F12_DATA_BYTES ((FIXTURE_ROOT_SECTOR + 7u) * 512u)

#define IOLOG_MAX 512

typedef struct {
    uint64_t off;
    size_t len;
} IoCall;

/* Custom counting backend: fat_io_t embedded as the FIRST member (the
 * documented downcast idiom) over a caller-owned image buffer.  Counts
 * and logs every transfer, flags out-of-bounds attempts, and can inject
 * read/write failures for the error-propagation tests. */
typedef struct {
    fat_io_t io;             /* must stay first */
    uint8_t* data;           /* caller-owned */
    size_t size;
    int read_calls, write_calls, close_calls;
    int oob;                 /* transfers that escaped [0, size) */
    IoCall reads[IOLOG_MAX];
    IoCall writes[IOLOG_MAX];
    int nreads, nwrites;
    int log_overflow;
    int fail_reads_from_call; /* calls with index >= this fail (-1 never) */
    uint64_t fail_reads_at;   /* reads with offset >= this fail */
    int fail_writes;          /* every write fails */
    int fail_writes_from_call; /* writes with index >= this fail (-1 never) */
} CountIo;

/* backend rules from fat.h: zero-length transfers succeed
 * unconditionally, transfers past size() fail with FAT_ERR_IO and copy
 * nothing */
static fat_result_t countio_read(fat_io_t* io, uint64_t offset, void* buf,
                                 size_t len)
{
    CountIo* c = (CountIo*)io;
    int call = c->read_calls++;

    if (c->nreads < IOLOG_MAX) {
        c->reads[c->nreads].off = offset;
        c->reads[c->nreads].len = len;
        c->nreads++;
    } else {
        c->log_overflow = 1;
    }
    if (len == 0)
        return FAT_OK;
    if (offset > c->size || len > c->size - (size_t)offset) {
        c->oob++;
        return FAT_ERR_IO;
    }
    if ((c->fail_reads_from_call >= 0 && call >= c->fail_reads_from_call) ||
        offset >= c->fail_reads_at)
        return FAT_ERR_IO;
    memcpy(buf, c->data + (size_t)offset, len);
    return FAT_OK;
}

static fat_result_t countio_write(fat_io_t* io, uint64_t offset,
                                  const void* buf, size_t len)
{
    CountIo* c = (CountIo*)io;
    int call = c->write_calls++;
    if (c->nwrites < IOLOG_MAX) {
        c->writes[c->nwrites].off = offset;
        c->writes[c->nwrites].len = len;
        c->nwrites++;
    } else {
        c->log_overflow = 1;
    }
    if (len == 0)
        return FAT_OK;
    if (offset > c->size || len > c->size - (size_t)offset) {
        c->oob++;
        return FAT_ERR_IO;
    }
    if (c->fail_writes ||
        (c->fail_writes_from_call >= 0 && call >= c->fail_writes_from_call))
        return FAT_ERR_IO;
    memcpy(c->data + (size_t)offset, buf, len);
    return FAT_OK;
}

static uint64_t countio_size(const fat_io_t* io)
{
    return ((const CountIo*)io)->size;
}

static void countio_close(fat_io_t* io)
{
    ((CountIo*)io)->close_calls++;
}

static void countio_init(CountIo* c, uint8_t* data, size_t size)
{
    memset(c, 0, sizeof(*c));
    c->io.read = countio_read;
    c->io.write = countio_write;
    c->io.size = countio_size;
    c->io.close = countio_close;
    c->data = data;
    c->size = size;
    c->fail_reads_from_call = -1;
    c->fail_reads_at = UINT64_MAX;
    c->fail_writes_from_call = -1;
}

/* 1 when any logged call's byte range overlaps [lo, hi) */
static int log_touches(const IoCall* calls, int n, uint64_t lo, uint64_t hi)
{
    for (int i = 0; i < n; i++)
        if (calls[i].off < hi && calls[i].off + calls[i].len > lo)
            return 1;
    return 0;
}

static uint64_t log_bytes(const IoCall* calls, int n)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++)
        sum += calls[i].len;
    return sum;
}

static void write_image(const char* name, const uint8_t* buf, size_t size)
{
    FILE* fp = fopen(name, "wb");
    if (fp == NULL)
        perror(name);
    assert(fp != NULL);
    assert(fwrite(buf, 1, size, fp) == size);
    fclose(fp);
}

/* The fat_io_t embedding idiom and the documented backend rules, checked
 * on our own backend before the library gets involved: size() reports
 * the fixed extent, zero-length transfers succeed, transfers past
 * size() fail with FAT_ERR_IO and copy nothing. */
static void test_io_backend_semantics(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* buf = copy_image(orig, size);
    CountIo io;
    countio_init(&io, buf, size);

    fat_io_t* vtbl = &io.io; /* plain pointer at the embedded vtable */
    assert(vtbl->read != NULL && vtbl->write != NULL);
    assert(vtbl->size(vtbl) == size);

    uint8_t tmp[8];
    memset(tmp, 0xAA, sizeof(tmp));
    assert(vtbl->read(vtbl, 0, tmp, 0) == FAT_OK);
    assert(vtbl->write(vtbl, 0, tmp, 0) == FAT_OK);
    assert(vtbl->read(vtbl, 100, tmp, sizeof(tmp)) == FAT_OK);
    assert(memcmp(tmp, buf + 100, sizeof(tmp)) == 0);

    assert(vtbl->read(vtbl, size - 1, tmp, 2) == FAT_ERR_IO);
    assert(vtbl->write(vtbl, size, "x", 1) == FAT_ERR_IO);
    assert(vtbl->read(vtbl, size + 512, tmp, 1) == FAT_ERR_IO);
    assert(io.read_calls == 4);
    assert(io.write_calls == 2);
    assert(io.oob == 3);
    /* the failed transfers did not touch the buffer */
    assert(memcmp(tmp, buf + 100, sizeof(tmp)) == 0);

    vtbl->close(vtbl);
    assert(io.close_calls == 1);

    free(buf);
    free(orig);
}

/* fat_open_io through a custom backend: the call pattern the library
 * issues (on-demand region reads, never a whole-image load, no backend
 * writes while read-only) and the exactly-once close at fat_close. */
static void test_open_io_calls(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* buf = copy_image(orig, size);
    CountIo io;
    countio_init(&io, buf, size);
    fat_ctx_t* ctx = NULL;

    assert(fat_open_io(&io.io, &ctx) == FAT_OK);
    assert(ctx != NULL);
    /* "the image is not loaded whole" (fat.h): open reads the boot
     * sector, not the whole 720KB image */
    assert(log_bytes(io.reads, io.nreads) < size);
    assert(io.write_calls == 0);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 12);
    /* the dirent bytes can only come from the fixed root region */
    assert(log_touches(io.reads, io.nreads, F12_ROOT_BYTES, F12_DATA_BYTES));
    assert(io.write_calls == 0); /* reads alone never flush anything */
    assert(io.oob == 0);

    uint32_t v = 0;
    assert(fat_get_fat_entry(ctx, 3, &v) == FAT_OK);
    assert(v == 4);
    assert(log_touches(io.reads, io.nreads, FIXTURE_FAT_SECTOR * 512u,
                       F12_ROOT_BYTES));

    fat_close(ctx);
    assert(io.close_calls == 1); /* exactly once, at fat_close */
    assert(io.oob == 0);

    /* close() == NULL is legal (nothing to release): fat_close must
     * not call it */
    CountIo nc;
    countio_init(&nc, buf, size);
    nc.io.close = NULL;
    fat_ctx_t* nctx = NULL;
    assert(fat_open_io(&nc.io, &nctx) == FAT_OK);
    assert(fat_lookup(nctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    fat_close(nctx);

    free(buf);
    free(orig);
}

/* fat_open_io failure paths: an invalid BPB leaves the backend with the
 * caller (no close call, ownership not stolen); a backend too small to
 * hold the boot sector fails the BPB read itself -> FAT_ERR_IO (16.6). */
static void test_open_io_fail_paths(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);

    uint8_t* buf = copy_image(orig, size);
    memset(buf + BPB_SIGNATURE, 0, 2);
    CountIo io;
    countio_init(&io, buf, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(&io.io, &ctx) == FAT_ERR_INVALID_BPB);
    assert(io.close_calls == 0); /* the context never took ownership */
    io.io.close(&io.io);         /* the caller releases it themselves */
    assert(io.close_calls == 1);
    free(buf);

    CountIo tiny;
    countio_init(&tiny, orig, 100);
    fat_ctx_t* tctx = NULL;
    assert(fat_open_io(&tiny.io, &tctx) == FAT_ERR_IO);
    assert(tiny.close_calls == 0);
    tiny.io.close(&tiny.io);

    free(orig);
}

/* Write-back visibility through a custom backend.  FAT[53] is chosen so
 * the two mirrored FAT sectors (1 and 4) occupy distinct slots even in
 * the 16-slot direct-mapped cache: no eviction can flush early, so
 * "backend unchanged until sync" is guaranteed, not incidental.  (A
 * fat_write_file here would also touch data sector 116, which shares
 * slot 4 with FAT table #1 and could evict-flush mid-op per 16.2 -- the
 * write path itself is covered by test_io_file_roundtrip instead.) */
static void test_sync_writeback(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* buf = copy_image(orig, size);
    uint8_t* snap = copy_image(orig, size);
    CountIo io;
    countio_init(&io, buf, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(&io.io, &ctx) == FAT_OK);

    /* the write lives in the cache: byte-identical backend, no io->write
     * call, but already visible through the context */
    assert(fat_set_fat_entry(ctx, 53, 0x235) == FAT_OK);
    assert(fat_at(ctx, 53) == 0x235);
    assert(io.write_calls == 0);
    assert(memcmp(buf, snap, size) == 0);

    /* sync flushes: both FAT tables carry the new value in their bytes */
    assert(fat_sync(ctx) == FAT_OK);
    assert(io.write_calls > 0);
    assert(raw_fat12_at(buf, FIXTURE_FAT_SECTOR, 53) == 0x235);
    assert(raw_fat12_at(buf, 4u, 53) == 0x235);
    assert(io.close_calls == 0); /* sync does not release anything */

    /* a second write is held again, and fat_close flushes it */
    memcpy(snap, buf, size);
    assert(fat_set_fat_entry(ctx, 53, 0x146) == FAT_OK);
    assert(fat_at(ctx, 53) == 0x146);
    assert(memcmp(buf, snap, size) == 0);
    fat_close(ctx);
    assert(io.close_calls == 1);
    assert(raw_fat12_at(buf, FIXTURE_FAT_SECTOR, 53) == 0x146);
    assert(raw_fat12_at(buf, 4u, 53) == 0x146);

    /* the flushed backend equals the whole-image export of a context
     * that performed the same operation (fat_write ground truth) */
    fat_ctx_t* ref = NULL;
    assert(fat_open_mem(orig, size, &ref) == FAT_OK);
    assert(fat_set_fat_entry(ref, 53, 0x146) == FAT_OK);
    assert(fat_write(ref, "/tmp/p5_wb_ref.fat") == FAT_OK);
    size_t rn = 0;
    uint8_t* want = read_image("/tmp/p5_wb_ref.fat", &rn);
    assert(rn == size);
    assert(memcmp(buf, want, size) == 0);
    remove("/tmp/p5_wb_ref.fat");
    fat_close(ref);

    free(want);
    free(snap);
    free(buf);
    free(orig);
}

/* fat_io_mem as a standalone backend over a private copy: the caller's
 * buffer is never written, and a synced flush is visible through the
 * backend's own vtable. */
static void test_io_mem_backend(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* snap = copy_image(orig, size);

    fat_io_t* io = NULL;
    assert(fat_io_mem(&io, orig, size) == FAT_OK);
    assert(io != NULL);
    assert(io->size(io) == size);

    uint8_t tmp[16];
    assert(io->read(io, 0, tmp, sizeof(tmp)) == FAT_OK);
    assert(memcmp(tmp, orig, sizeof(tmp)) == 0);

    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(io, &ctx) == FAT_OK);
    assert(fat_set_fat_entry(ctx, 53, 0x235) == FAT_OK);
    assert(fat_sync(ctx) == FAT_OK);

    /* entry 53 is odd: FAT bytes 79..80 decode as (b0>>4)|(b1<<4) */
    uint8_t e[2];
    assert(io->read(io, FIXTURE_FAT_SECTOR * 512u + 53u * 3u / 2u, e,
                    sizeof(e)) == FAT_OK);
    assert((uint16_t)((e[0] >> 4) | ((uint16_t)e[1] << 4)) == 0x235);

    /* the caller's original buffer survived it all */
    assert(memcmp(orig, snap, size) == 0);

    fat_close(ctx); /* owns io now: flushed and closed exactly once */
    free(snap);
    free(orig);
}

/* fat_io_file on a real file (a /tmp copy -- never a fixture): writes
 * reach the file at sync/close, the on-disk result equals the fat_write
 * export of the same operations, and a fresh fat_open reads it back. */
static void test_io_file_roundtrip(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);

    fat_io_t* nope = NULL;
    assert(fat_io_file(&nope, "/tmp/p5_no_such_backend.fat") == FAT_ERR_IO);

    write_image("/tmp/p5_fio_rt.fat", orig, size);
    fat_io_t* io = NULL;
    assert(fat_io_file(&io, "/tmp/p5_fio_rt.fat") == FAT_OK);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(io, &ctx) == FAT_OK);

    size_t n = 5000;
    uint8_t* data = make_pattern(n);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);
    assert(fat_sync(ctx) == FAT_OK);
    fat_close(ctx); /* close flushes too; nothing may be dirty now */

    /* expected on-disk state: the same write, exported */
    fat_ctx_t* ref = NULL;
    assert(fat_open_mem(orig, size, &ref) == FAT_OK);
    assert(fat_write_file(ref, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);
    assert(fat_write(ref, "/tmp/p5_fio_ref.fat") == FAT_OK);
    size_t gn = 0;
    size_t rn = 0;
    uint8_t* got = read_image("/tmp/p5_fio_rt.fat", &gn);
    uint8_t* want = read_image("/tmp/p5_fio_ref.fat", &rn);
    assert(gn == size && rn == size);
    assert(memcmp(got, want, size) == 0);

    /* a fresh fat_open (file backend internally) reads the result */
    fat_ctx_t* fresh = NULL;
    assert(fat_open("/tmp/p5_fio_rt.fat", &fresh) == FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(fresh, FAT_CLUSTER_ROOT, "new.txt", &de) == FAT_OK);
    assert(de.file_size == n);
    uint8_t* back = NULL;
    size_t bn = 0;
    assert(fat_read_file(fresh, &de, &back, &bn) == FAT_OK);
    assert(bn == n);
    assert(memcmp(back, data, n) == 0);
    free(back);
    assert_read_string(fresh, "hello.txt", "hello world\n");
    fat_close(fresh);

    remove("/tmp/p5_fio_rt.fat");
    remove("/tmp/p5_fio_ref.fat");
    fat_close(ref);
    free(got);
    free(want);
    free(data);
    free(orig);
}

/* Backend write failures surface at fat_sync as FAT_ERR_IO; the cache
 * still holds the write, and fat_close stays best-effort and closes the
 * backend exactly once anyway. */
static void test_write_fail_inject(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* buf = copy_image(orig, size);
    CountIo io;
    countio_init(&io, buf, size);
    io.fail_writes = 1;
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(&io.io, &ctx) == FAT_OK);

    assert(fat_set_fat_entry(ctx, 53, 0x235) == FAT_OK);
    assert(fat_at(ctx, 53) == 0x235); /* cached; the flush comes later */
    assert(fat_sync(ctx) == FAT_ERR_IO);
    assert(io.write_calls > 0); /* the backend was actually asked */

    fat_close(ctx); /* void: best-effort flush, no double close */
    assert(io.close_calls == 1);

    free(buf);
    free(orig);
}

/* Backend read failures propagate as FAT_ERR_IO from the invoking
 * public API (16.6), through three injection points: every post-BPB
 * read, the fixed root region, and the data region (lookup is fine). */
static void test_read_fail_inject(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);

    {
        /* the BPB read is call #0; everything after that fails */
        uint8_t* buf = copy_image(orig, size);
        CountIo io;
        countio_init(&io, buf, size);
        io.fail_reads_from_call = 1;
        fat_ctx_t* ctx = NULL;
        assert(fat_open_io(&io.io, &ctx) == FAT_OK);
        uint32_t v = 0;
        assert(fat_get_fat_entry(ctx, 2, &v) == FAT_ERR_IO);
        fat_close(ctx);
        assert(io.close_calls == 1);
        free(buf);
    }
    {
        /* root region unreadable: iterating the fixed root must fail */
        uint8_t* buf = copy_image(orig, size);
        CountIo io;
        countio_init(&io, buf, size);
        io.fail_reads_at = F12_ROOT_BYTES;
        fat_ctx_t* ctx = NULL;
        assert(fat_open_io(&io.io, &ctx) == FAT_OK);
        int n = 0;
        assert(fat_iter_dir(ctx, FAT_CLUSTER_ROOT, count_any_cb, &n) ==
               FAT_ERR_IO);
        fat_close(ctx);
        assert(io.close_calls == 1);
        free(buf);
    }
    {
        /* data region unreadable: the lookup works, the read fails, and
         * no buffer escapes on the error path */
        uint8_t* buf = copy_image(orig, size);
        CountIo io;
        countio_init(&io, buf, size);
        io.fail_reads_at = F12_DATA_BYTES;
        fat_ctx_t* ctx = NULL;
        assert(fat_open_io(&io.io, &ctx) == FAT_OK);
        fat_dirent_t de;
        memset(&de, 0, sizeof(de));
        assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
        assert(de.file_size == 12);
        uint8_t* data = NULL;
        size_t nn = 0;
        assert(fat_read_file(ctx, &de, &data, &nn) == FAT_ERR_IO);
        assert(data == NULL);
        fat_close(ctx);
        assert(io.close_calls == 1);
        free(buf);
    }

    free(orig);
}

/* FAT12 entry straddling a sector boundary.  Cluster 341 packs at
 * 341*3/2 = 511 (floor division), i.e. FAT bytes 511..512 = image bytes
 * 1023..1024: the last byte of sector 1 and the first byte of sector 2.
 * Writing it must read-modify-write across both cache slots and keep
 * the nibble neighbours 340 (shares byte 511) and 342 (shares byte 512)
 * intact, in both FAT tables. */
static void test_fat12_straddle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* buf = copy_image(orig, size);
    CountIo io;
    countio_init(&io, buf, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(&io.io, &ctx) == FAT_OK);
    const fat_geometry_t* g = fat_geometry(ctx);

    /* preconditions measured on the fixture: the FAT table covers byte
     * 513 (3 sectors = 1536 bytes) and 342 is a valid data cluster */
    assert(F12_ROOT_BYTES - FIXTURE_FAT_SECTOR * 512u >= 513u);
    assert(g->cluster_count + 1u >= 342u);
    assert(fat_at(ctx, 340) == 0);
    assert(fat_at(ctx, 341) == 0);
    assert(fat_at(ctx, 342) == 0);

    assert(fat_set_fat_entry(ctx, 340, 0x123) == FAT_OK);
    assert(fat_set_fat_entry(ctx, 342, 0x456) == FAT_OK);
    assert(fat_set_fat_entry(ctx, 341, 0xABC) == FAT_OK);
    assert(fat_at(ctx, 340) == 0x123);
    assert(fat_at(ctx, 341) == 0xABC);
    assert(fat_at(ctx, 342) == 0x456);

    /* flush and byte-verify both FAT tables (base sectors 1 and 4): the
     * neighbours decode exactly, which is only possible when the
     * two-sector RMW preserved every shared nibble */
    assert(fat_sync(ctx) == FAT_OK);
    for (unsigned t = 0; t < 2u; t++) {
        unsigned base = t == 0u ? FIXTURE_FAT_SECTOR : 4u;
        assert(raw_fat12_at(buf, base, 340) == 0x123);
        assert(raw_fat12_at(buf, base, 341) == 0xABC);
        assert(raw_fat12_at(buf, base, 342) == 0x456);
    }
    /* the straddled pair itself: image byte 1023 carries 0x123's high
     * nibble below 0xABC's low nibble, byte 1024 the rest of 0xABC */
    assert(buf[1023] ==
           (uint8_t)(((0x123u >> 8) & 0x0Fu) | ((0xABCu & 0x0Fu) << 4)));
    assert(buf[1024] == (uint8_t)(0xABCu >> 4));

    fat_close(ctx);
    assert(io.close_calls == 1);

    free(buf);
    free(orig);
}

/* The 16.5 semantics change through plain fat_open, old symbols only:
 * writes reach the file itself at fat_close.  Phase 4 kept them in RAM
 * and left the file untouched -- that is the red this test pins down.
 * Works on a /tmp copy; fixtures are never written through fat_open. */
static void test_open_write_reaches_file(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    write_image("/tmp/p5_reach.fat", orig, size);

    size_t n = 100;
    uint8_t* data = make_pattern(n);

    fat_ctx_t* ctx = NULL;
    assert(fat_open("/tmp/p5_reach.fat", &ctx) == FAT_OK);
    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);
    fat_close(ctx); /* the flush point */

    /* expected on-disk bytes: the same operation, exported */
    fat_ctx_t* ref = NULL;
    assert(fat_open_mem(orig, size, &ref) == FAT_OK);
    assert(fat_write_file(ref, FAT_CLUSTER_ROOT, "new.txt", data, n,
                          NULL) == FAT_OK);
    assert(fat_write(ref, "/tmp/p5_reach_ref.fat") == FAT_OK);
    size_t gn = 0;
    size_t rn = 0;
    uint8_t* got = read_image("/tmp/p5_reach.fat", &gn);
    uint8_t* want = read_image("/tmp/p5_reach_ref.fat", &rn);
    assert(gn == size && rn == size);
    assert(memcmp(got, want, size) == 0);

    /* and a fresh reader sees the written file */
    fat_ctx_t* fresh = NULL;
    assert(fat_open("/tmp/p5_reach.fat", &fresh) == FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(fresh, FAT_CLUSTER_ROOT, "new.txt", &de) == FAT_OK);
    assert(de.file_size == n);
    assert_read_string(fresh, "hello.txt", "hello world\n");
    fat_close(fresh);

    remove("/tmp/p5_reach.fat");
    remove("/tmp/p5_reach_ref.fat");
    fat_close(ref);
    free(got);
    free(want);
    free(data);
    free(orig);
}

/* ------------------------------------------------------------------ */
/* phase 6: deletion + write cursors (fat_unlink, fat_rmdir,           */
/* fat_file_open_write, fat_file_truncate, fat_file_write)             */
/* ------------------------------------------------------------------ */

/* byte offset of the live slot whose on-disk 11-byte name matches,
 * inside a raw image region (the fixed root or one directory cluster);
 * 0 when absent (region offsets are sector-aligned, hence nonzero) */
static size_t find_slot_in_region(const uint8_t* img, size_t region_off,
                                  unsigned n_slots, const char* name11)
{
    for (unsigned i = 0; i < n_slots; i++)
        if (memcmp(img + region_off + i * 32u, name11, 11) == 0)
            return region_off + i * 32u;
    return 0;
}

/* the directory slot of `name` as the 32 raw on-disk bytes, read
 * through the context (the sector cache), like fat_dir_next hands out */
static void read_slot_raw(fat_ctx_t* ctx, uint32_t dir_cluster,
                          const char* name, uint8_t out[32])
{
    uint8_t want11[11];
    assert(fat_name_to_83(name, want11) == FAT_OK);
    fat_dir_t* d = NULL;
    assert(fat_dir_open(ctx, dir_cluster, &d) == FAT_OK);
    int found = 0;
    while (!found) {
        const fat_dirent_t* e = NULL;
        const uint8_t* r = NULL;
        fat_result_t rr = fat_dir_next(d, &e, &r);
        assert(rr == FAT_OK || rr == FAT_ERR_END_OF_DIR);
        if (rr != FAT_OK)
            break;
        if (e->name[0] == '.')
            continue; /* dot entries are not 8.3 names */
        uint8_t got11[11];
        assert(fat_name_to_83(e->name, got11) == FAT_OK);
        if (memcmp(got11, want11, 11) == 0) {
            memcpy(out, r, 32);
            found = 1;
        }
    }
    fat_dir_close(d);
    assert(found);
}

/* slot integrity (18.2): only the first-cluster (bytes 20-21, 26-27)
 * and file-size (28-31) fields may differ -- name, attributes and every
 * timestamp stay byte-identical (truncate/write touch no timestamps). */
static void assert_slot_only_size_cluster_changed(const uint8_t before[32],
                                                  const uint8_t after[32],
                                                  uint32_t want_cluster,
                                                  uint32_t want_size)
{
    assert(memcmp(before, after, 20) == 0);          /* name..lstAccDate */
    assert(memcmp(before + 22, after + 22, 4) == 0); /* wrtTime/wrtDate */
    uint32_t cluster = ((uint32_t)after[20] << 16) |
                       ((uint32_t)after[21] << 24) |
                       (uint32_t)after[26] |
                       ((uint32_t)after[27] << 8);
    uint32_t size = (uint32_t)after[28] | ((uint32_t)after[29] << 8) |
                    ((uint32_t)after[30] << 16) | ((uint32_t)after[31] << 24);
    assert(cluster == want_cluster);
    assert(size == want_size);
}

/* first `n` bytes of the committed host fixture file */
static void read_host_prefix(size_t n, uint8_t* buf)
{
    FILE* fp = fopen("test_5kb.txt", "rb");
    if (fp == NULL)
        perror("test_5kb.txt");
    assert(fp != NULL);
    assert(fread(buf, 1, n, fp) == n);
    fclose(fp);
}

/* the chain clusters from `first` into out[]; returns the length */
static uint32_t record_chain(const fat_ctx_t* ctx, uint32_t first,
                             uint32_t* out, uint32_t max)
{
    uint32_t n = 0;
    uint32_t c = first;
    while (c >= 2u && n < max) {
        out[n++] = c;
        c = fat_at(ctx, c);
        if (c <= 1u || c > fat_geometry(ctx)->cluster_count + 1u)
            break; /* EOC / broken */
    }
    return n;
}

/* 18.1: unlink marks the slot 0xE5, frees the chain (FAT entries 0,
 * free count back), leaves every neighbour byte-identical, and works
 * in subdirectories too (hoge.txt in dir1). */
static void test_unlink_basic12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint32_t free_before = count_free_clusters(ctx);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de) == FAT_OK);
    uint32_t hoge_cluster = de.first_cluster;
    assert(hoge_cluster >= 2u);

    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, 2) == 0); /* hello's single cluster is free again */
    assert(count_free_clusters(ctx) == free_before + 1u);
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 4);

    /* same story inside dir1: hoge.txt (single cluster) */
    assert(fat_unlink(ctx, 8, "hoge.txt") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, hoge_cluster) == 0);
    assert(count_free_clusters(ctx) == free_before + 2u);
    assert(count_dir_entries(ctx, 8) == 4); /* . .. subdir1 subdir2 */

    /* raw bytes on the flushed image: both slots now start with 0xE5,
     * every other root slot is byte-identical */
    assert(fat_write(ctx, "/tmp/p6_unlink12.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_unlink12.fat", &fn);
    assert(fn == size);
    size_t hello_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    size_t label_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                           "DEMOF12    ");
    size_t kb_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                        "TEST_5KBTXT");
    size_t dir1_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                          "DIR1       ");
    size_t dir2_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                          "DIR2       ");
    size_t hoge_off = find_slot_in_region(orig, 26u * 512u, 32,
                                          "HOGE    TXT");
    assert(hello_off && label_off && kb_off && dir1_off && dir2_off);
    assert(hoge_off != 0);
    assert(back[hello_off] == 0xE5);
    assert(back[hoge_off] == 0xE5);
    assert(memcmp(back + label_off, orig + label_off, 32) == 0);
    assert(memcmp(back + kb_off, orig + kb_off, 32) == 0);
    assert(memcmp(back + dir1_off, orig + dir1_off, 32) == 0);
    assert(memcmp(back + dir2_off, orig + dir2_off, 32) == 0);
    free(back);
    remove("/tmp/p6_unlink12.fat");

    /* the multi-cluster neighbour still reads byte-exact */
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);

    fat_close(ctx);
    free(orig);
}

/* 16.5/18.1 round trip: unlink through a real file backend, fat_sync,
 * close, then a fresh fat_open sees the deletion persisted and the other
 * files intact.  Works on a /tmp copy; fixtures are never written. */
static void test_unlink_sync_reopen(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    write_image("/tmp/p6_sync.fat", orig, size);
    size_t hello_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    assert(hello_off != 0);

    fat_io_t* io = NULL;
    assert(fat_io_file(&io, "/tmp/p6_sync.fat") == FAT_OK);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(io, &ctx) == FAT_OK);
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt") == FAT_OK);
    assert(fat_sync(ctx) == FAT_OK);
    fat_close(ctx);

    /* the deletion reached the file itself */
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_sync.fat", &fn);
    assert(fn == size);
    assert(back[hello_off] == 0xE5);
    free(back);

    fat_ctx_t* fresh = NULL;
    assert(fat_open("/tmp/p6_sync.fat", &fresh) == FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(fresh, FAT_CLUSTER_ROOT, "hello.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(count_dir_entries(fresh, FAT_CLUSTER_ROOT) == 4);
    assert_read_matches_host(fresh, "test_5kb.txt", "test_5kb.txt", 4962);
    assert(fat_lookup(fresh, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert(de.first_cluster == 8);
    fat_close(fresh);

    remove("/tmp/p6_sync.fat");
    free(orig);
}

/* 18.1 error paths: NOT_FOUND, directory target, ATTR_READ_ONLY, NULL
 * arguments -- and none of them may disturb the image. */
static void test_unlink_errors12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    fat_dirent_t de;

    assert(fat_unlink(NULL, FAT_CLUSTER_ROOT, "hello.txt") ==
           FAT_ERR_INVALID_ARG);

    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "noexist.txt") ==
           FAT_ERR_NOT_FOUND);
    /* directories go through fat_rmdir */
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "dir1") == FAT_ERR_INVALID_ARG);
    assert(fat_unlink(ctx, 8, "subdir1") == FAT_ERR_INVALID_ARG);
    /* the volume label never matches a lookup */
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "demof12") == FAT_ERR_NOT_FOUND);
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, NULL) == FAT_ERR_INVALID_ARG);

    /* the failures changed nothing */
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5);
    assert(count_dir_entries(ctx, 8) == 5);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    fat_close(ctx);

    /* ATTR_READ_ONLY: the DOS access-denied equivalent */
    img = copy_image(orig, size);
    size_t hello_off = find_slot_in_region(img, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    assert(hello_off != 0);
    img[hello_off + 11] |= 0x01;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt") ==
           FAT_ERR_INVALID_ARG);
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 12);
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5);
    fat_close(ctx);

    free(orig);
}

/* 18.1: an empty file (first_cluster 0) loses only its dirent slot --
 * no cluster accounting moves at all. */
static void test_unlink_empty_file12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    assert(fat_write_file(ctx, FAT_CLUSTER_ROOT, "empty.txt", NULL, 0,
                          NULL) == FAT_OK);
    uint32_t free_before = count_free_clusters(ctx);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "empty.txt", &de) == FAT_OK);
    assert(de.first_cluster == 0);

    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "empty.txt") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "empty.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(count_free_clusters(ctx) == free_before); /* nothing to free */
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5); /* back to 5 */

    /* raw: the slot the empty file took (first 0x00 slot = #5, right
     * after DIR2) is a deleted slot now */
    assert(fat_write(ctx, "/tmp/p6_undel12.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_undel12.fat", &fn);
    assert(back[F12_ROOT_BYTES + 5u * 32u] == 0xE5);
    free(back);
    remove("/tmp/p6_undel12.fat");

    fat_close(ctx);
    free(orig);
}

/* 18.1 on FAT32: the FSInfo free count climbs by the freed cluster
 * count and the hint stays sane; multi-cluster chains free entirely. */
static void test_unlink_fat32(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    /* hello.txt: one cluster */
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, 3) == 0);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u + 1u);
    assert(fi.next_free_cluster >= 2u);
    assert(fi.next_free_cluster <= g->cluster_count + 1u);

    /* test_5kb.txt: the 10-cluster chain 4..13 frees entirely */
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt") == FAT_OK);
    for (uint32_t c = 4u; c <= 13u; c++)
        assert(fat_at(ctx, c) == 0);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u + 11u);
    /* the FSInfo now tells the truth of the FAT itself */
    assert(count_free_clusters(ctx) == 66454u + 11u);

    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 42);

    fat_close(ctx);
    free(orig);
}

/* 18.1 x 12.4: the 0xE5 slot our own unlink produced is the first slot
 * fat_add_dirent reuses (the phase-4 priority rule), byte offset
 * included. */
static void test_unlink_slot_reuse(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    size_t hello_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    assert(hello_off != 0);
    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt") == FAT_OK);

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    tmpl.first_cluster = 2;
    tmpl.file_size = 8;
    assert(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "fresh.txt", &tmpl) ==
           FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "fresh.txt", &de) == FAT_OK);
    assert(de.first_cluster == 2);
    assert(de.file_size == 8);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) ==
           FAT_ERR_NOT_FOUND);

    /* raw: FRESH sits exactly where HELLO was */
    assert(fat_write(ctx, "/tmp/p6_reuse6.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_reuse6.fat", &fn);
    size_t fresh_off = find_slot_in_region(back, F12_ROOT_BYTES, 112,
                                           "FRESH   TXT");
    assert(fresh_off == hello_off);
    free(back);
    remove("/tmp/p6_reuse6.fat");

    /* the slot count is back to 5 */
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5);

    fat_close(ctx);
    free(orig);
}

/* 18.1: rmdir of an empty subdirectory -- slot 0xE5, chain freed, the
 * neighbours in the parent untouched. */
static void test_rmdir_empty12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir1", &de) == FAT_OK);
    uint32_t sub1 = de.first_cluster;
    assert(fat_at(ctx, sub1) == 0xFFF); /* single-cluster directory */

    uint32_t free_before = count_free_clusters(ctx);
    assert(fat_rmdir(ctx, 8, "subdir1") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir1", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, sub1) == 0); /* the directory cluster is free */
    assert(count_free_clusters(ctx) == free_before + 1u);
    assert(count_dir_entries(ctx, 8) == 4); /* . .. subdir2 hoge */

    /* raw: SUBDIR1's slot in dir1 is 0xE5, every other slot identical */
    assert(fat_write(ctx, "/tmp/p6_rmdir12.fat") == FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_rmdir12.fat", &fn);
    size_t dir1_off = 26u * 512u; /* cluster 8 = data sector 26 */
    size_t sub1_off = find_slot_in_region(orig, dir1_off, 32, "SUBDIR1    ");
    size_t sub2_off = find_slot_in_region(orig, dir1_off, 32, "SUBDIR2    ");
    size_t hoge_off = find_slot_in_region(orig, dir1_off, 32, "HOGE    TXT");
    assert(sub1_off && sub2_off && hoge_off);
    assert(back[sub1_off] == 0xE5);
    assert(memcmp(back + sub2_off, orig + sub2_off, 32) == 0);
    assert(memcmp(back + hoge_off, orig + hoge_off, 32) == 0);
    assert(memcmp(back + dir1_off, orig + dir1_off, 64) == 0); /* "." ".." */
    free(back);
    remove("/tmp/p6_rmdir12.fat");

    fat_close(ctx);
    free(orig);
}

/* 18.1 error paths: DIR_NOT_EMPTY, file targets, the root itself (from
 * "." and from a child's ".."), NOT_FOUND, label, NULL. */
static void test_rmdir_errors12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    fat_dirent_t de;

    /* dir1 holds subdir1, subdir2, hoge.txt */
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "dir1") == FAT_ERR_DIR_NOT_EMPTY);
    /* dir2/subdir1 holds page.txt and test_5kb.txt */
    assert(fat_rmdir(ctx, 11, "subdir1") == FAT_ERR_DIR_NOT_EMPTY);
    /* regular files are not directories */
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "hello.txt") ==
           FAT_ERR_INVALID_ARG);
    /* the root cannot be removed, however it is named */
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, ".") == FAT_ERR_INVALID_ARG);
    assert(fat_rmdir(ctx, 8, "..") == FAT_ERR_INVALID_ARG);
    /* lookup failures propagate */
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "noexist") == FAT_ERR_NOT_FOUND);
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "demof12") == FAT_ERR_NOT_FOUND);
    assert(fat_rmdir(NULL, FAT_CLUSTER_ROOT, "dir1") == FAT_ERR_INVALID_ARG);
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, NULL) == FAT_ERR_INVALID_ARG);

    /* nothing above changed the image */
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 5);
    assert(count_dir_entries(ctx, 8) == 5);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de) == FAT_OK);
    assert(de.first_cluster == 8);
    assert(fat_at(ctx, 8) == 0xFFF);
    assert_read_string(ctx, "hello.txt", "hello world\n");

    fat_close(ctx);
    free(orig);
}

/* 18.1: deleted 0xE5 slots (and the 0x00 tail) do not make a directory
 * non-empty.  One variant with raw-mutated 0xE5 junk slots, one where
 * our own unlink produced them. */
static void test_rmdir_deleted_slots12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);

    /* dir1/subdir2 (cluster 10 = data sector 30): junk after ".." --
     * two deleted slots, then the 0x00 terminator again */
    size_t sub2_off = 30u * 512u;
    memcpy(img + sub2_off + 2u * 32u, "\xE5JUNKONE   ", 11);
    memcpy(img + sub2_off + 3u * 32u, "\xE5JUNKTWO   ", 11);
    img[sub2_off + 4u * 32u] = 0x00;

    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir2", &de) == FAT_OK);
    uint32_t sub2 = de.first_cluster;
    assert(fat_at(ctx, sub2) == 0xFFF);

    assert(fat_rmdir(ctx, 8, "subdir2") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir2", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, sub2) == 0);

    /* composed variant: dir2/subdir1 holds two files; unlinking both
     * leaves only "."/".." plus 0xE5 slots, and rmdir accepts that */
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &de) == FAT_OK);
    uint32_t d2s1 = de.first_cluster;
    assert(fat_unlink(ctx, d2s1, "page.txt") == FAT_OK);
    assert(fat_unlink(ctx, d2s1, "test_5kb.txt") == FAT_OK);
    assert(count_dir_entries(ctx, d2s1) == 2); /* just the dots */
    assert(fat_rmdir(ctx, 11, "subdir1") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, d2s1) == 0);

    fat_close(ctx);
    free(orig);
}

/* 18.1 on FAT32: rmdir frees the directory cluster and keeps FSInfo in
 * step.  dir1/sub1 (cluster 57) is empty. */
static void test_rmdir_fat32(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de) == FAT_OK);
    assert(de.first_cluster == 57);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);

    /* the fixture's sub1 holds page.txt (cluster 58) -- phase 2 pins that.
     * unlink it first, then the dots-only directory is removable */
    assert(fat_unlink(ctx, 57, "page.txt") == FAT_OK);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u + 1u);

    assert(fat_rmdir(ctx, 56, "sub1") == FAT_OK);
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_at(ctx, 57) == 0);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u + 2u);

    /* non-empty still refuses: dir1 itself */
    assert(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "dir1") == FAT_ERR_DIR_NOT_EMPTY);

    fat_close(ctx);
    free(orig);
}

/* 18.2: open_write binds to an existing regular file and reads exactly
 * like the read cursor; NOT_FOUND / directory / READ_ONLY / NULL are
 * refused and leave no cursor behind. */
static void test_open_write_basic12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    fat_dirent_t de;
    fat_file_t* f = NULL;

    assert(fat_file_open_write(NULL, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_ERR_INVALID_ARG);

    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "noexist.txt", &f) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "dir1", &f) ==
           FAT_ERR_INVALID_ARG);
    assert(fat_file_open_write(ctx, 8, "subdir1", &f) == FAT_ERR_INVALID_ARG);
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "demof12", &f) ==
           FAT_ERR_NOT_FOUND);
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, NULL, &f) ==
           FAT_ERR_INVALID_ARG);

    /* success: cursor facts and read behaviour of fat_file_open */
    f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_OK);
    assert(f != NULL);
    assert(fat_file_tell(f) == 0);
    assert(fat_file_size(f) == 12);
    uint8_t buf[16];
    size_t got = 0x5A5A;
    assert(fat_file_read(f, buf, sizeof(buf), &got) == FAT_OK);
    assert(got == 12);
    assert(memcmp(buf, "hello world\n", 12) == 0);
    assert(fat_file_tell(f) == 12);
    assert(fat_file_seek(f, 0) == FAT_OK);
    got = 0x5A5A;
    assert(fat_file_read(f, buf, 12, &got) == FAT_OK);
    assert(got == 12);
    assert(memcmp(buf, "hello world\n", 12) == 0);
    assert(fat_file_seek(f, 13) == FAT_ERR_INVALID_ARG); /* past EOF */
    fat_file_close(f);
    fat_close(ctx);

    /* ATTR_READ_ONLY refuses (the write side of the DOS semantics) */
    img = copy_image(orig, size);
    size_t hello_off = find_slot_in_region(img, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    assert(hello_off != 0);
    img[hello_off + 11] |= 0x01;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_ERR_INVALID_ARG);
    fat_close(ctx);

    /* a subdirectory binding reads the file in that directory */
    img = copy_image(orig, size);
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);
    f = NULL;
    assert(fat_file_open_write(ctx, 12, "page.txt", &f) == FAT_OK);
    assert(fat_file_size(f) == 14);
    got = 0x5A5A;
    assert(fat_file_read(f, buf, 14, &got) == FAT_OK);
    assert(got == 14);
    assert(memcmp(buf, "You are page.\n", 14) == 0);
    fat_file_close(f);
    fat_close(ctx);

    (void)de;
    free(orig);
}

/* 18.2 shrink: dirent size first, then the tail clusters free; the
 * cursor clamps to the new size; the kept bytes still read back; only
 * the size/cluster fields of the slot change.  Target: the copy of
 * test_5kb.txt inside dir2/subdir1 (chain 48..52) -- a different slot
 * and chain than the root one. */
static void test_truncate_shrink12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, 12, "test_5kb.txt", slot_before);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/test_5kb.txt",
                      &de) == FAT_OK);
    uint32_t chain[8];
    uint32_t nchain = record_chain(ctx, de.first_cluster, chain, 8);
    assert(nchain == 5); /* 4962 bytes over 1024B clusters */
    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, 12, "test_5kb.txt", &f) == FAT_OK);
    assert(fat_file_size(f) == 4962);
    assert(fat_file_seek(f, 4000) == FAT_OK);

    assert(fat_file_truncate(f, 1000) == FAT_OK);
    assert(fat_file_size(f) == 1000);
    assert(fat_file_tell(f) == 1000); /* clamped down from 4000 */

    /* the chain is one cluster now; the four tail clusters are FREE */
    assert(fat_at(ctx, chain[0]) == 0xFFF);
    for (uint32_t i = 1; i < nchain; i++)
        assert(fat_at(ctx, chain[i]) == 0);
    assert(count_free_clusters(ctx) == free_before + 4u);

    /* the kept prefix still reads back byte-exact through the cursor */
    assert(fat_file_seek(f, 0) == FAT_OK);
    uint8_t* want = malloc(1000);
    uint8_t* buf = malloc(1000);
    assert(want != NULL && buf != NULL);
    read_host_prefix(1000, want);
    size_t got = 0;
    assert(fat_file_read(f, buf, 1000, &got) == FAT_OK);
    assert(got == 1000);
    assert(memcmp(buf, want, 1000) == 0);
    /* EOF right after */
    got = 0x5A5A;
    assert(fat_file_read(f, buf, 1, &got) == FAT_OK);
    assert(got == 0);
    free(want);
    free(buf);

    /* a position below the new size keeps its offset */
    assert(fat_file_seek(f, 10) == FAT_OK);
    assert(fat_file_truncate(f, 500) == FAT_OK);
    assert(fat_file_tell(f) == 10);
    assert(fat_file_size(f) == 500);
    fat_file_close(f);

    /* the slot changed in exactly the two sanctioned fields */
    uint8_t slot_after[32];
    read_slot_raw(ctx, 12, "test_5kb.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, 500);

    /* and a fresh lookup agrees */
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/test_5kb.txt",
                      &de) == FAT_OK);
    assert(de.file_size == 500);
    assert(de.first_cluster == chain[0]);

    /* the root copy is untouched */
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    assert(de.file_size == 4962);
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);

    fat_close(ctx);
    free(orig);
}

/* 18.2 grow: the new bytes read back as zeros through the same cursor,
 * the chain extends by whole clusters, and the slot keeps its cluster
 * with only the size field moving. */
static void test_truncate_grow12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_before);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    uint32_t chain[16];
    uint32_t nchain = record_chain(ctx, de.first_cluster, chain, 16);
    assert(nchain == 5);
    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f) ==
           FAT_OK);
    assert(fat_file_truncate(f, 6000) == FAT_OK);
    assert(fat_file_size(f) == 6000);
    assert(fat_file_tell(f) == 0);

    /* 6000 bytes = 6 clusters of 1024: one new cluster, chain linked */
    assert(chain_length(ctx, de.first_cluster) == 6);
    assert(fat_at(ctx, chain[nchain - 1]) != 0xFFF); /* old tail links on */
    assert(count_free_clusters(ctx) == free_before - 1u);

    /* whole content: the original 4962 bytes, then 1038 zeros */
    assert(fat_file_seek(f, 0) == FAT_OK);
    uint8_t* buf = malloc(6000);
    uint8_t* want = malloc(6000);
    assert(buf != NULL && want != NULL);
    size_t got = 0;
    assert(fat_file_read(f, buf, 6000, &got) == FAT_OK);
    assert(got == 6000);
    read_host_prefix(4962, want);
    memset(want + 4962, 0, 1038);
    assert(memcmp(buf, want, 6000) == 0);
    free(want);
    free(buf);

    /* zeros strictly past the old end, through a seek too */
    assert(fat_file_seek(f, 4962) == FAT_OK);
    uint8_t z = 0xAA;
    got = 0x5A5A;
    assert(fat_file_read(f, &z, 1, &got) == FAT_OK);
    assert(got == 1);
    assert(z == 0);
    fat_file_close(f);

    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, 6000);

    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    assert(de.file_size == 6000);

    fat_close(ctx);
    free(orig);
}

/* 18.2 truncate(0): the whole chain frees and the on-disk slot shows
 * first_cluster 0 / size 0 like a freshly created empty file; growing
 * that empty file allocates a new chain. */
static void test_truncate_zero12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_before);

    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_OK);
    assert(fat_file_seek(f, 5) == FAT_OK);
    assert(fat_file_truncate(f, 0) == FAT_OK);
    assert(fat_file_size(f) == 0);
    assert(fat_file_tell(f) == 0); /* clamped to 0 */
    assert(fat_at(ctx, 2) == 0);   /* hello's only cluster freed */
    assert(count_free_clusters(ctx) == free_before + 1u);

    /* empty reads EOF */
    uint8_t b = 0x41;
    size_t got = 0x5A5A;
    assert(fat_file_read(f, &b, 1, &got) == FAT_OK);
    assert(got == 0);

    /* the slot: first_cluster 0 ON DISK, everything else untouched */
    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after, 0, 0);

    /* growing the empty file allocates a fresh chain */
    assert(fat_file_truncate(f, 2000) == FAT_OK);
    assert(fat_file_size(f) == 2000);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 2000);
    assert(de.first_cluster >= 2u); /* a real chain was allocated */
    assert(de.first_cluster != 2u || fat_at(ctx, 2) != 0);
    assert(chain_length(ctx, de.first_cluster) == 2);
    assert(count_free_clusters(ctx) == free_before - 1u);

    /* and it reads back as zeros */
    assert(fat_file_seek(f, 0) == FAT_OK);
    uint8_t* buf = malloc(2000);
    assert(buf != NULL);
    memset(buf, 0x5A, 2000);
    got = 0;
    assert(fat_file_read(f, buf, 2000, &got) == FAT_OK);
    assert(got == 2000);
    for (int i = 0; i < 2000; i++)
        assert(buf[i] == 0);
    free(buf);
    fat_file_close(f);

    fat_close(ctx);
    free(orig);
}

/* 18.2 on FAT32: FSInfo tracks the shrink and the grow exactly, and the
 * zero-fill reads back through the same cursor. */
static void test_truncate_fat32(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_OK);
    assert(fat_file_size(f) == 12); /* one 512B cluster */

    /* grow to 1500 = 3 clusters: two allocations */
    assert(fat_file_truncate(f, 1500) == FAT_OK);
    assert(fat_file_size(f) == 1500);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u - 2u);
    assert(fat_file_seek(f, 12) == FAT_OK);
    uint8_t z[1488];
    memset(z, 0x5A, sizeof(z));
    size_t got = 0;
    assert(fat_file_read(f, z, sizeof(z), &got) == FAT_OK);
    assert(got == sizeof(z));
    for (size_t i = 0; i < sizeof(z); i++)
        assert(z[i] == 0);

    /* shrink to 100 = 1 cluster: both extensions free again */
    assert(fat_file_seek(f, 1400) == FAT_OK);
    assert(fat_file_truncate(f, 100) == FAT_OK);
    assert(fat_file_tell(f) == 100); /* clamped */
    assert(fat_file_size(f) == 100);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);
    fat_file_close(f);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 100);
    assert(de.first_cluster == 3); /* the original cluster survived */

    fat_close(ctx);
    free(orig);
}

/* 18.2 write: a same-size in-place overwrite replaces the content,
 * moves neither size nor cluster accounting, and honours the
 * no-short-write contract (including the len == 0 call). */
static void test_write_inplace12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_before);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    assert(chain_length(ctx, de.first_cluster) == 5);
    uint32_t free_before = count_free_clusters(ctx);

    size_t n = 4962;
    uint8_t* data = make_pattern(n);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f) ==
           FAT_OK);

    /* len == 0: OK, nothing written, nothing moved */
    size_t w = 0x5A5A;
    assert(fat_file_write(f, data, 0, &w) == FAT_OK);
    assert(w == 0);
    assert(fat_file_tell(f) == 0);
    assert(fat_file_size(f) == n);

    assert(fat_file_write(f, data, n, &w) == FAT_OK);
    assert(w == n); /* no short writes */
    assert(fat_file_tell(f) == n);
    assert(fat_file_size(f) == n); /* size unchanged */

    /* same five clusters, no allocation, no free */
    assert(chain_length(ctx, de.first_cluster) == 5);
    assert(count_free_clusters(ctx) == free_before);

    /* read-back through the same cursor */
    assert(fat_file_seek(f, 0) == FAT_OK);
    uint8_t* buf = malloc(n);
    assert(buf != NULL);
    size_t got = 0;
    assert(fat_file_read(f, buf, n, &got) == FAT_OK);
    assert(got == n);
    assert(memcmp(buf, data, n) == 0);
    free(buf);
    fat_file_close(f);

    /* and through a fresh whole-file read */
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    uint8_t* back = NULL;
    size_t bn = 0;
    assert(fat_read_file(ctx, &de, &back, &bn) == FAT_OK);
    assert(bn == n);
    assert(memcmp(back, data, n) == 0);
    free(back);

    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, (uint32_t)n);

    free(data);
    fat_close(ctx);
    free(orig);
}

/* 18.2 write past EOF: the file extends, clusters allocate, the cursor
 * lands at the new end, and the prefix (old content) survives. */
static void test_write_extend12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_before);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.first_cluster == 2);
    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_OK);
    assert(fat_file_seek(f, 12) == FAT_OK); /* EOF is a legal position */

    size_t n = 3000;
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    assert(fat_file_write(f, data, n, &w) == FAT_OK);
    assert(w == n);
    assert(fat_file_tell(f) == 3012);
    assert(fat_file_size(f) == 3012);
    assert(chain_length(ctx, de.first_cluster) == 3); /* 3012B/1024B */
    assert(count_free_clusters(ctx) == free_before - 2u);

    /* content: the greeting, then the pattern */
    assert(fat_file_seek(f, 0) == FAT_OK);
    uint8_t* buf = malloc(3012);
    assert(buf != NULL);
    size_t got = 0;
    assert(fat_file_read(f, buf, 3012, &got) == FAT_OK);
    assert(got == 3012);
    assert(memcmp(buf, "hello world\n", 12) == 0);
    assert(memcmp(buf + 12, data, n) == 0);
    free(buf);
    fat_file_close(f);

    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, 3012);

    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 3012);

    free(data);
    fat_close(ctx);
    free(orig);
}

/* 18.2 seek into the middle and overwrite 100 bytes across the first
 * cluster boundary (1024): size and cluster accounting stay put. */
static void test_write_middle12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    uint32_t free_before = count_free_clusters(ctx);

    uint8_t* patch = make_pattern(100);
    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f) ==
           FAT_OK);
    assert(fat_file_seek(f, 1000) == FAT_OK);
    size_t w = 0x5A5A;
    assert(fat_file_write(f, patch, 100, &w) == FAT_OK);
    assert(w == 100);
    assert(fat_file_tell(f) == 1100);
    assert(fat_file_size(f) == 4962); /* size unchanged */
    assert(chain_length(ctx, de.first_cluster) == 5);
    assert(count_free_clusters(ctx) == free_before);

    /* expected: host[0..1000) + patch + host[1100..4962) */
    uint8_t* want = malloc(4962);
    uint8_t* buf = malloc(4962);
    assert(want != NULL && buf != NULL);
    read_host_prefix(4962, want);
    memcpy(want + 1000, patch, 100);
    assert(fat_file_seek(f, 0) == FAT_OK);
    size_t got = 0;
    assert(fat_file_read(f, buf, 4962, &got) == FAT_OK);
    assert(got == 4962);
    assert(memcmp(buf, want, 4962) == 0);
    free(want);
    free(buf);
    fat_file_close(f);

    /* the whole-file read agrees */
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    uint8_t* back = NULL;
    size_t bn = 0;
    assert(fat_read_file(ctx, &de, &back, &bn) == FAT_OK);
    assert(bn == 4962);
    assert(memcmp(back + 1000, patch, 100) == 0);
    free(back);

    free(patch);
    fat_close(ctx);
    free(orig);
}

/* 18.2 + 16.6: with a backend whose writes all fail, an extending
 * fat_file_write must surface FAT_ERR_IO and leave the file consistent
 * with the prefix it reports (dirent size == old size + *written).
 * The construction guarantees a cache eviction -- and thus a flush
 * attempt -- inside the call: the 4th new cluster's data sector (36)
 * and FAT table #1 (sector 4) share direct-mapped cache slot 4, so
 * whichever is written second evicts a dirty slot. */
static void test_write_fail_io12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* buf = copy_image(orig, size);
    CountIo io;
    countio_init(&io, buf, size);
    io.fail_writes_from_call = 0; /* every backend write fails */
    fat_ctx_t* ctx = NULL;
    assert(fat_open_io(&io.io, &ctx) == FAT_OK);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f) ==
           FAT_OK);
    assert(fat_file_seek(f, 4962) == FAT_OK); /* write at EOF */

    size_t n = 3300; /* 4962+3300 = 8262 B = 9 clusters: 4 new ones */
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    fat_result_t r = fat_file_write(f, data, n, &w);
    assert(r == FAT_ERR_IO);
    assert(w <= n);
    assert(io.write_calls > 0); /* the backend was actually asked */
    assert(io.oob == 0);

    /* consistency: the reported size covers exactly the prefix (no lookups
     * afterwards -- reading a fresh sector could evict another dirty slot
     * and fail on its own, which a correct implementation may freely do) */
    assert(fat_file_size(f) == 4962u + w);

    /* the failed flushes stay dirty: sync retries and fails again */
    assert(fat_sync(ctx) == FAT_ERR_IO);

    fat_file_close(f);
    fat_close(ctx);
    assert(io.close_calls == 1);
    free(data);
    free(buf);
    free(orig);
}

/* 18.2 prefix consistency on DISK_FULL: with only two free clusters
 * left, an extending write reports FAT_ERR_DISK_FULL and leaves the
 * landed prefix readable: size == 4962 + *written and exactly those
 * bytes come back (old content, then the pattern prefix).  The two
 * free clusters hold at most bytes 4962..7167, so w <= 2206. */
static void test_write_disk_full_prefix12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);

    /* fill every free FAT entry except clusters 300 and 301 */
    for (uint32_t c = 2u; c <= 713u + 1u; c++) {
        if (c == 300u || c == 301u)
            continue;
        if (raw_fat12_at(img, FIXTURE_FAT_SECTOR, c) == 0)
            set_fat12_entry(img, c, 0x001);
    }
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f) ==
           FAT_OK);
    assert(fat_file_seek(f, 4962) == FAT_OK);

    size_t n = 3300; /* needs 4 new clusters, only 2 are free */
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    assert(fat_file_write(f, data, n, &w) == FAT_ERR_DISK_FULL);
    assert(w <= 2206u);
    assert(fat_file_size(f) == 4962u + w);

    /* the prefix is consistent and readable end to end */
    assert(fat_file_seek(f, 0) == FAT_OK);
    size_t total = 4962u + w;
    uint8_t* buf = malloc(total);
    uint8_t* want = malloc(total);
    assert(buf != NULL && want != NULL);
    read_host_prefix(4962, want);
    if (w > 0)
        memcpy(want + 4962, data, w);
    size_t got = 0;
    assert(fat_file_read(f, buf, total, &got) == FAT_OK);
    assert(got == total);
    assert(memcmp(buf, want, total) == 0);
    free(buf);
    free(want);
    fat_file_close(f);

    /* a fresh lookup agrees on the truncated-but-consistent size */
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de) == FAT_OK);
    assert(de.file_size == 4962u + w);

    free(data);
    fat_close(ctx);
    free(orig);
}

/* 18.2: NULL/partial arguments behave like fat_file_read's
 * (INVALID_ARG), and a zero-length write is a clean no-op. */
static void test_write_null_args12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    uint8_t b = 'x';
    size_t w = 0x5A5A;
    assert(fat_file_write(NULL, &b, 1, &w) == FAT_ERR_INVALID_ARG);
    assert(fat_file_truncate(NULL, 0) == FAT_ERR_INVALID_ARG);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_OK);
    assert(fat_file_write(f, NULL, 1, &w) == FAT_ERR_INVALID_ARG);
    assert(fat_file_write(f, &b, 1, NULL) == FAT_ERR_INVALID_ARG);
    /* the rejected calls moved nothing */
    assert(fat_file_tell(f) == 0);
    assert(fat_file_size(f) == 12);
    w = 0x5A5A;
    assert(fat_file_write(f, &b, 0, &w) == FAT_OK);
    assert(w == 0);
    assert(fat_file_size(f) == 12);
    fat_file_close(f);

    fat_close(ctx);
    free(orig);
}

/* 18.2 on FAT32: write past EOF steps FSInfo down by the new clusters,
 * truncate steps it back up, and the content round-trips. */
static void test_write_fat32(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);

    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f) ==
           FAT_OK);
    assert(fat_file_size(f) == 12);
    assert(fat_file_seek(f, 12) == FAT_OK);

    size_t n = 2000;
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    assert(fat_file_write(f, data, n, &w) == FAT_OK);
    assert(w == n);
    assert(fat_file_size(f) == 2012); /* 2012 B = 4 clusters of 512 */
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u - 3u);

    /* read-back: the greeting, then the pattern */
    assert(fat_file_seek(f, 0) == FAT_OK);
    uint8_t* back = malloc(2012);
    assert(back != NULL);
    size_t got = 0;
    assert(fat_file_read(f, back, 2012, &got) == FAT_OK);
    assert(got == 2012);
    assert(memcmp(back, "hello world\n", 12) == 0);
    assert(memcmp(back + 12, data, n) == 0);
    free(back);

    /* truncate back to one cluster and watch the FSInfo reverse */
    assert(fat_file_truncate(f, 512) == FAT_OK);
    assert(fat_file_size(f) == 512);
    assert(fat_fsinfo(ctx, &fi) == FAT_OK);
    assert(fi.free_cluster_count == 66454u);
    fat_file_close(f);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) == FAT_OK);
    assert(de.file_size == 512);
    assert(de.first_cluster == 3);

    free(data);
    fat_close(ctx);
    free(orig);
}

/* 18.4 oracle: after our unlink, mdir -b agrees on the listing and
 * mtype still delivers a survivor byte-exact. */
static void test_mtools_unlink_oracle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    assert(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt") == FAT_OK);

    assert(fat_write(ctx, "/tmp/p6_orch12.fat") == FAT_OK);
    assert_listing_matches_mdir(ctx, "/tmp/p6_orch12.fat", "",
                                FAT_CLUSTER_ROOT);
    assert_mtype_matches_read(ctx, "/tmp/p6_orch12.fat", "test_5kb.txt");
    remove("/tmp/p6_orch12.fat");

    fat_close(ctx);
    free(orig);
}

/* 18.4 oracle, the other direction: an image mtools deleted from must
 * read back through our APIs exactly as mdir/mtype see it.  (Uses no
 * phase-6 API: a regression lock, green in the red phase by design.) */
static void test_mtools_mdel_oracle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    write_image("/tmp/p6_mdel.fat", orig, size);
    free(orig);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mdel -i /tmp/p6_mdel.fat ::hello.txt");
    assert(system(cmd) == 0);

    fat_ctx_t* ctx = NULL;
    assert(fat_open("/tmp/p6_mdel.fat", &ctx) == FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    assert(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de) ==
           FAT_ERR_NOT_FOUND);
    assert(count_dir_entries(ctx, FAT_CLUSTER_ROOT) == 4);
    assert_listing_matches_mdir(ctx, "/tmp/p6_mdel.fat", "", FAT_CLUSTER_ROOT);
    assert_mtype_matches_read(ctx, "/tmp/p6_mdel.fat", "test_5kb.txt");
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);
    fat_close(ctx);

    remove("/tmp/p6_mdel.fat");
}

/* 18.4 oracle: our cursor overwrite vs mtype, and mcopy -o's overwrite
 * vs our read. */
static void test_mtools_overwrite_oracle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    assert(fat_open_mem(img, size, &ctx) == FAT_OK);
    free(img);

    /* our partial overwrite: 200 bytes at offset 1000 */
    fat_file_t* f = NULL;
    assert(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f) ==
           FAT_OK);
    assert(fat_file_seek(f, 1000) == FAT_OK);
    uint8_t* patch = make_pattern(200);
    size_t w = 0x5A5A;
    assert(fat_file_write(f, patch, 200, &w) == FAT_OK);
    assert(w == 200);
    fat_file_close(f);
    free(patch);

    assert(fat_write(ctx, "/tmp/p6_orcw.fat") == FAT_OK);
    assert_mtype_matches_read(ctx, "/tmp/p6_orcw.fat", "test_5kb.txt");
    fat_close(ctx);
    remove("/tmp/p6_orcw.fat");

    /* mtools' overwrite: mcopy -o the host file over hello.txt */
    write_image("/tmp/p6_orcw.fat", orig, size);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "mcopy -o -i /tmp/p6_orcw.fat test_5kb.txt ::hello.txt");
    assert(system(cmd) == 0);

    fat_ctx_t* fresh = NULL;
    assert(fat_open("/tmp/p6_orcw.fat", &fresh) == FAT_OK);
    assert_read_matches_host(fresh, "hello.txt", "test_5kb.txt", 4962);
    assert_mtype_matches_read(fresh, "/tmp/p6_orcw.fat", "hello.txt");
    fat_close(fresh);
    remove("/tmp/p6_orcw.fat");
    free(orig);
}

/* ------------------------------------------------------------------ */
/* fixture protection (16.5): no test may modify demof12/16/32.fat --  */
/* writes go through fat_open_mem, custom backends or /tmp copies.     */
/* FNV-1a snapshots taken first thing, re-checked last thing.          */
/* ------------------------------------------------------------------ */

static uint64_t fixture_hash(const char* name)
{
    FILE* fp = fopen(name, "rb");
    if (fp == NULL)
        perror(name);
    assert(fp != NULL);
    uint8_t buf[65536];
    uint64_t h = 0xcbf29ce484222325ull; /* FNV-1a 64 */
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        for (size_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 0x100000001b3ull;
        }
    }
    fclose(fp);
    return h;
}

static uint64_t guard_hash[3];

static void test_fixture_guard_begin(void)
{
    const char* names[3] = {IMG_NAME, IMG16_NAME, IMG32_NAME};
    for (int i = 0; i < 3; i++)
        guard_hash[i] =
            fixture_present(names[i]) ? fixture_hash(names[i]) : 0;
}

static void test_fixture_guard_end(void)
{
    const char* names[3] = {IMG_NAME, IMG16_NAME, IMG32_NAME};
    for (int i = 0; i < 3; i++) {
        if (guard_hash[i] == 0)
            continue; /* absent at start: not guarded (and not written) */
        uint64_t h = fixture_hash(names[i]);
        if (h != guard_hash[i])
            fprintf(stderr, "%s was modified by the test suite\n", names[i]);
        assert(h == guard_hash[i]);
    }
}

int main(void)
{
    RUN(test_fixture_guard_begin);

    RUN(test_open);
    RUN(test_fat_entries);
    RUN(test_root_iterate);
    RUN(test_dir2_iterate);
    RUN(test_names);
    RUN(test_name_05_escape);
    RUN(test_lookup);
    RUN(test_lookup_root_dots);
    RUN(test_lookup_volume_label);
    RUN(test_read_file);
    RUN(test_ctx_lifecycle);
    RUN(test_open_mem_errors);

    RUN(test_dos_date_to_tm);
    RUN(test_file_stream);
    RUN(test_dir_cursor);
    RUN(test_boot_fuzz_fat12);
    if (mtools_present()) {
        RUN(test_mtools_diff);
    } else {
        printf("%-40s skipped (mtools not installed)\n", "test_mtools_diff");
    }

    RUN(test_set_fat_entry12);
    RUN(test_alloc_cluster12);
    RUN(test_free_chain12);
    RUN(test_add_dirent_create);
    RUN(test_add_dirent_reuse);
    RUN(test_add_dirent_errors);
    RUN(test_write_file_roundtrip12);
    RUN(test_write_file_boundaries);
    RUN(test_write_file_small);
    RUN(test_write_file_rollback);
    RUN(test_write_file_subdir);
    RUN(test_write_flush);

    if (fixture_present(IMG16_NAME)) {
        RUN(test_fat16_open);
        RUN(test_fat16_fat_entries);
        RUN(test_fat16_root_iterate);
        RUN(test_fat16_lookup_read);
        RUN(test_fat16_label_lookup);
        RUN(test_fat16_file_stream);
        RUN(test_fat16_dir_cursor);
        RUN(test_boot_fuzz_values_fat16);
        RUN(test_fat16_write);
        if (mtools_present()) {
            RUN(test_fat16_mtools_diff);
        } else {
            printf("%-40s skipped (mtools not installed)\n",
                   "test_fat16_mtools_diff");
        }
    } else {
        printf("%-40s skipped (%s missing; run: make fat16)\n",
               "FAT16 suite", IMG16_NAME);
    }

    if (fixture_present(IMG32_NAME)) {
        RUN(test_fat32_open);
        RUN(test_fat32_fat_entries);
        RUN(test_fat32_root_iterate);
        RUN(test_fat32_lookup_read);
        RUN(test_fat32_label_lookup);
        RUN(test_fat32_fsinfo);
        RUN(test_open_mem_errors_fat32);
        RUN(test_fat32_file_stream);
        RUN(test_fat32_dir_cursor);
        RUN(test_boot_fuzz_values_fat32);
        RUN(test_set_fat_entry32);
        RUN(test_alloc_free32);
        RUN(test_fat32_root_extend);
        RUN(test_fat32_write_file);
        if (mtools_present()) {
            RUN(test_fat32_mtools_diff);
        } else {
            printf("%-40s skipped (mtools not installed)\n",
                   "test_fat32_mtools_diff");
        }
    } else {
        printf("%-40s skipped (%s missing; run: make fat32)\n",
               "FAT32 suite", IMG32_NAME);
    }

    /* phase 5: I/O abstraction */
    RUN(test_io_backend_semantics);
    RUN(test_open_io_calls);
    RUN(test_open_io_fail_paths);
    RUN(test_sync_writeback);
    RUN(test_io_mem_backend);
    RUN(test_io_file_roundtrip);
    RUN(test_write_fail_inject);
    RUN(test_read_fail_inject);
    RUN(test_fat12_straddle);
    RUN(test_open_write_reaches_file);

    /* phase 6: deletion + write cursors */
    RUN(test_unlink_basic12);
    RUN(test_unlink_sync_reopen);
    RUN(test_unlink_errors12);
    RUN(test_unlink_empty_file12);
    RUN(test_unlink_slot_reuse);
    RUN(test_rmdir_empty12);
    RUN(test_rmdir_errors12);
    RUN(test_rmdir_deleted_slots12);
    RUN(test_open_write_basic12);
    RUN(test_truncate_shrink12);
    RUN(test_truncate_grow12);
    RUN(test_truncate_zero12);
    RUN(test_write_inplace12);
    RUN(test_write_extend12);
    RUN(test_write_middle12);
    RUN(test_write_fail_io12);
    RUN(test_write_disk_full_prefix12);
    RUN(test_write_null_args12);
    if (mtools_present()) {
        RUN(test_mtools_unlink_oracle);
        RUN(test_mtools_mdel_oracle);
        RUN(test_mtools_overwrite_oracle);
    } else {
        printf("%-40s skipped (mtools not installed)\n",
               "test_mtools_unlink_oracle");
        printf("%-40s skipped (mtools not installed)\n",
               "test_mtools_mdel_oracle");
        printf("%-40s skipped (mtools not installed)\n",
               "test_mtools_overwrite_oracle");
    }
    if (fixture_present(IMG32_NAME)) {
        RUN(test_unlink_fat32);
        RUN(test_rmdir_fat32);
        RUN(test_truncate_fat32);
        RUN(test_write_fat32);
    } else {
        printf("%-40s skipped (%s missing; run: make fat32)\n",
               "phase 6 FAT32 suite", IMG32_NAME);
    }

    RUN(test_fixture_guard_end);

    return 0;
}
