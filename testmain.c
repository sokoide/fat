/* Phase 2 test suite: exercises the public fat.h API against three frozen
 * mtools-generated fixtures.  All fixture facts asserted here were verified
 * independently with mtools (mdir/minfo) and raw image dumps (od):
 *
 * demof12.fat (720KB FAT12, mformat -f 720) -- committed to the repo:
 *   root: DEMOF12 label (attr 0x08), HELLO.TXT (cluster 2, 12 bytes),
 *         TEST_5KB.TXT (cluster 3, 4962 bytes, chain 3->4->5->6->7),
 *         DIR1 (cluster 8), DIR2 (cluster 11, chain 11->43)
 *   dir1: ".", "..", SUBDIR1 (empty), SUBDIR2 (empty), HOGE.TXT (11 bytes)
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

int main(void)
{
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

    if (fixture_present(IMG16_NAME)) {
        RUN(test_fat16_open);
        RUN(test_fat16_fat_entries);
        RUN(test_fat16_root_iterate);
        RUN(test_fat16_lookup_read);
        RUN(test_fat16_label_lookup);
        RUN(test_fat16_file_stream);
        RUN(test_fat16_dir_cursor);
        RUN(test_boot_fuzz_values_fat16);
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

    return 0;
}
