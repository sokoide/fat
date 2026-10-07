/* test_read.c -- the read-side test group of the suite: open/geometry,
 * FAT chain walks, directory iteration, 8.3 name rendering, lookup,
 * whole-file and streaming reads, and the boot-sector fuzz regressions,
 * across the three fixtures (demof12/fat16/fat32).  The fixture facts
 * asserted here are documented in testmain.c's header comment; the shared
 * framework and helpers live in test_util.h.  Split out of the
 * monolithic testmain.c. */

#include "test_util.h"

#include <time.h>

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
    /* Lead adjudication (§28.3, phase 10): the fixture stores these 8.3
     * names with NTRes lowercase flags, so dirent.name now renders in
     * lowercase -- the on-disk flags applied, not a regression */
    if (strcmp(entry->name, "hello.txt") == 0) {
        counts->saw_hello = 1;
        EXPECT_EQ(entry->first_cluster, 2);
        EXPECT_EQ(entry->file_size, 12);
    } else if (strcmp(entry->name, "test_5kb.txt") == 0) {
        counts->saw_5kb = 1;
        EXPECT_EQ(entry->first_cluster, 3);
        EXPECT_EQ(entry->file_size, 4962);
    } else if (strcmp(entry->name, "dir1") == 0) {
        counts->saw_dir1 = 1;
        EXPECT_EQ(entry->first_cluster, 8);
        EXPECT_TRUE(entry->attributes & 0x10);
    } else if (strcmp(entry->name, "dir2") == 0) {
        counts->saw_dir2 = 1;
        EXPECT_EQ(entry->first_cluster, 11);
        EXPECT_TRUE(entry->attributes & 0x10);
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
    int dots;      /* "." and ".." (delivered by iteration, not skipped) */
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
    /* Lead adjudication (§28.3, phase 10): the dir2 subdirs carry NTRes
     * 0x08 and now render as "subdirN" */
    if (strncmp(entry->name, "subdir", 6) != 0)
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
    EXPECT_EQ(fat_open("no_such_image.fat", &ctx), FAT_ERR_IO);

    ctx = open_fixture();

    /* geometry: fatStart=1, rootDir=1+2*3=7, rootDirSectors=ceil(112*32/512)=7,
     * dataStart=7+7=14, clusterCount=(0x5A0-14)/2=713 */
    const fat_geometry_t* g = fat_geometry(ctx);
    EXPECT_TRUE(g != NULL);
    EXPECT_EQ(g->bytes_per_sector, 512);
    EXPECT_EQ(g->sectors_per_cluster, 2);
    EXPECT_EQ(g->reserved_sectors, 1);
    EXPECT_EQ(g->fat_count, 2);
    EXPECT_EQ(g->fat_sectors, 3);
    EXPECT_EQ(g->root_entries, 0x70);
    EXPECT_EQ(g->total_sectors, 0x5A0);
    EXPECT_EQ(g->fat_start_sector, 1);
    EXPECT_EQ(g->root_dir_sector, 7);
    EXPECT_EQ(g->root_dir_sectors, 7);
    EXPECT_EQ(g->root_cluster, 0); /* FAT12/16: fixed root region, no chain */
    EXPECT_EQ(g->data_start_sector, 14);
    EXPECT_EQ(g->cluster_count, (0x5A0u - 14u) / 2u);
    EXPECT_EQ(g->cluster_count, 713);

    EXPECT_EQ(fat_get_type(ctx), FT_FAT12);
    EXPECT_EQ(fat_cluster_size(ctx), 512u * 2u);

    EXPECT_TRUE(fat_strerror(FAT_OK) != NULL);
    EXPECT_TRUE(fat_strerror(FAT_ERR_BAD_CLUSTER) != NULL);

    fat_close(ctx);
}

static void test_fat_entries(void)
{
    fat_ctx_t* ctx = open_fixture();

    /* media/reserved entries and the verified chains */
    EXPECT_EQ(fat_at(ctx, 0), 0xFF9);
    EXPECT_EQ(fat_at(ctx, 1), 0xFFF);
    EXPECT_EQ(fat_at(ctx, 2), 0xFFF); /* hello.txt: single cluster, EOC */
    EXPECT_EQ(fat_at(ctx, 3), 4);     /* test_5kb.txt chain 3->4->5->6->7 */
    EXPECT_EQ(fat_at(ctx, 4), 5);
    EXPECT_EQ(fat_at(ctx, 5), 6);
    EXPECT_EQ(fat_at(ctx, 6), 7);
    EXPECT_EQ(fat_at(ctx, 7), 0xFFF);
    EXPECT_EQ(fat_at(ctx, 8), 0xFFF); /* dir1: single cluster */
    EXPECT_EQ(fat_at(ctx, 11), 0x02B); /* dir2: cluster 11 continues at 43 */
    EXPECT_EQ(fat_at(ctx, 43), 0xFFF);

    /* indices 0..cluster_count+1 are readable, beyond that is an error */
    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t v = 0;
    EXPECT_EQ(fat_get_fat_entry(ctx, g->cluster_count + 1, &v), FAT_OK);
    EXPECT_EQ(fat_get_fat_entry(ctx, g->cluster_count + 2, &v),
              FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_root_iterate(void)
{
    fat_ctx_t* ctx = open_fixture();
    RootCounts counts = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    EXPECT_EQ(iter_dir(ctx, FAT_CLUSTER_ROOT, count_root, &counts),
              FAT_OK);
    /* DEMOF12 + hello.txt + test_5kb.txt + dir1 + dir2 */
    EXPECT_EQ(counts.labels, 1);
    EXPECT_EQ(counts.files, 2);
    EXPECT_EQ(counts.dirs, 2);
    EXPECT_EQ(counts.raw32_missing, 0);
    EXPECT_TRUE(counts.saw_label && counts.saw_hello && counts.saw_5kb);
    EXPECT_TRUE(counts.saw_dir1 && counts.saw_dir2);

    fat_close(ctx);
}

static void test_dir2_iterate(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 11);

    /* cluster 11 (full, 32 slots) -> cluster 43: 33 subdirs, no files */
    Dir2Counts counts = {0, 0, 0, 0};
    EXPECT_EQ(iter_dir(ctx, de.first_cluster, count_dir2, &counts),
              FAT_OK);
    EXPECT_EQ(counts.dots, 2);
    EXPECT_EQ(counts.subdirs, 33);
    EXPECT_EQ(counts.files, 0);
    EXPECT_EQ(counts.bad_name, 0);

    fat_close(ctx);
}

static void assert_to_83(const char* in, const char* want11)
{
    uint8_t name11[11];
    EXPECT_EQ(fat_name_to_83(in, name11), FAT_OK);
    EXPECT_MEMEQ(name11, want11, 11);
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
    EXPECT_EQ(fat_name_to_83("toolongname.txt", name11),
              FAT_ERR_NAME_TOO_LONG);
    EXPECT_EQ(fat_name_to_83("file.text", name11), FAT_ERR_NAME_TOO_LONG);
    /* rejected (fat.h does not pin the exact code for these three) */
    EXPECT_TRUE(fat_name_to_83("", name11) != FAT_OK);
    EXPECT_TRUE(fat_name_to_83(".", name11) != FAT_OK);
    EXPECT_TRUE(fat_name_to_83("..", name11) != FAT_OK);
    EXPECT_TRUE(fat_name_to_83(NULL, name11) != FAT_OK);

    /* from 8.3 round trip */
    EXPECT_EQ(fat_name_to_83("page.txt", name11), FAT_OK);
    EXPECT_EQ(fat_name_from_83(name11, 0x20, out, sizeof(out)), FAT_OK);
    EXPECT_STR_EQ(out, "PAGE.TXT");

    /* directories get no extension dot */
    memcpy(name11, "SUBDIR1    ", 11);
    EXPECT_EQ(fat_name_from_83(name11, 0x10, out, sizeof(out)), FAT_OK);
    EXPECT_STR_EQ(out, "SUBDIR1");

    /* volume labels get no extension dot either */
    memcpy(name11, "DEMOF12    ", 11);
    EXPECT_EQ(fat_name_from_83(name11, 0x08, out, sizeof(out)), FAT_OK);
    EXPECT_STR_EQ(out, "DEMOF12");

    /* extension trailing spaces are trimmed, all-blank ext means no dot */
    memcpy(name11, "FOO     A  ", 11);
    EXPECT_EQ(fat_name_from_83(name11, 0x20, out, sizeof(out)), FAT_OK);
    EXPECT_STR_EQ(out, "FOO.A");
    memcpy(name11, "README      ", 11);
    EXPECT_EQ(fat_name_from_83(name11, 0x20, out, sizeof(out)), FAT_OK);
    EXPECT_STR_EQ(out, "README");

    /* output buffer must hold the result plus NUL ("HELLO.TXT" = 10) */
    memcpy(name11, "HELLO   TXT", 11);
    EXPECT_EQ(fat_name_from_83(name11, 0x20, out, 9),
              FAT_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(fat_name_from_83(name11, 0x20, out, 10), FAT_OK);
    EXPECT_STR_EQ(out, "HELLO.TXT");
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

    EXPECT_EQ(fat_name_from_83(stored05, 0x20, out05, sizeof(out05)),
              FAT_OK);
    EXPECT_EQ(fat_name_from_83(storedE5, 0x20, outE5, sizeof(outE5)),
              FAT_OK);
    /* byte-exact render (0xE5 lead), and both forms agree */
    EXPECT_MEMEQ(out05, want, sizeof(want));
    EXPECT_MEMEQ(outE5, want, sizeof(want));
    EXPECT_MEMEQ(out05, outE5, sizeof(want));
}

/* phase 10 (§28): NTRes lowercase flags (dirent byte 12: bit3 base, bit4
 * extension) fold back into the rendered dirent.name, while fat_name_from_83
 * itself stays uppercase and matching stays case-insensitive. The frozen
 * fixture carries the flags on disk: hello.txt/test_5kb.txt are 0x18, dir1
 * is 0x08, the label is 0x00 */
static void test_nt_rendering(void)
{
    fat_ctx_t* ctx = open_fixture();

    fat_dir_t* d = NULL;
    EXPECT_EQ(fat_dir_open(ctx, FAT_CLUSTER_ROOT, &d), FAT_OK);
    const fat_dirent_t* entry;
    const uint8_t* raw32;
    int saw_hello = 0, saw_dir1 = 0, saw_label = 0;
    fat_result_t r;
    while ((r = fat_dir_next(d, &entry, &raw32)) == FAT_OK) {
        if (raw32[11] & 0x08) { /* volume label: NTRes 0x00, no folding */
            EXPECT_STR_EQ(entry->name, "DEMOF12");
            EXPECT_EQ(raw32[12], 0x00);
            saw_label = 1;
            continue;
        }
        if (strcmp(entry->name, "hello.txt") == 0) {
            EXPECT_EQ(raw32[12], 0x18); /* base + extension lowercase */
            EXPECT_EQ(entry->file_size, 12);
            saw_hello = 1;
        } else if (strcmp(entry->name, "dir1") == 0) {
            EXPECT_EQ(raw32[12], 0x08); /* base only: no extension */
            saw_dir1 = 1;
        }
    }
    EXPECT_EQ(r, FAT_ERR_END_OF_DIR);
    fat_dir_close(d);
    EXPECT_TRUE(saw_hello && saw_dir1 && saw_label);

    /* uppercase queries still resolve (ASCII case-insensitive matching) */
    fat_dirent_t de;
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "HELLO.TXT", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "Dir1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 8);

    /* the raw renderer is unchanged: flags are applied by dirent_from_raw
     * only, never by fat_name_from_83 itself */
    uint8_t name11[11];
    char out[FAT_NAME_MAX];
    memcpy(name11, "HELLO   TXT", 11);
    EXPECT_EQ(fat_name_from_83(name11, 0x20, out, sizeof(out)), FAT_OK);
    EXPECT_STR_EQ(out, "HELLO.TXT");

    fat_close(ctx);
}

static void test_lookup(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 8);
    EXPECT_TRUE(de.attributes & 0x10);

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 11);

    /* leading slash and empty components are ignored */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/dir2//subdir1", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 12);

    /* matching is case-insensitive (queries are folded to 8.3 upper) */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "HELLO.TXT", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de),
              FAT_OK);
    EXPECT_TRUE(de.first_cluster == 3 && de.file_size == 4962);

    /* deep paths, including the second copy inside dir2/subdir1 */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 11);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/page.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 14);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT,
                         "dir2/subdir1/test_5kb.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 4962);

    /* missing final component vs missing/not-a-directory intermediate */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/noexist", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "noexist/foo", &de),
              FAT_ERR_PATH_NOT_FOUND);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt/x", &de),
              FAT_ERR_PATH_NOT_FOUND);

    /* A2: "." / ".." at the root now succeed with a synthetic root dirent
     * (DOS semantics: the root is its own parent).  The fixed root region
     * has no dot entries; fat_lookup must synthesize one. */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, ".", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, ".");
    EXPECT_TRUE(de.attributes & 0x10);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_EQ(de.file_size, 0);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "..", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, "..");
    EXPECT_TRUE(de.attributes & 0x10);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_EQ(de.file_size, 0);

    /* "." resolves to the directory's own dot entry */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/.", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 8);
    EXPECT_TRUE(de.attributes & 0x10);

    /* ".." of a root-level directory yields the root cluster */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/..", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/../hello.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 12);

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
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/.", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "./..", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);

    /* down and back up past the root: dir1/.. lands on the root, and the
     * next ".." must stay there instead of failing */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/../..", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, "..");
    EXPECT_TRUE(de.attributes & 0x10);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_EQ(de.file_size, 0);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/../../hello.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "../../dir1/hoge.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 11);

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

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "demof12", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "DEMOF12", &de),
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
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/test_5kb.txt", &de),
              FAT_OK);
    data = &sentinel;
    size = 1;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_OK);
    EXPECT_TRUE(data != &sentinel);
    EXPECT_EQ(size, 4962);
    {
        FILE* fp = fopen("test_5kb.txt", "rb");
        if (fp == NULL)
            perror("test_5kb.txt");
        EXPECT_TRUE(fp != NULL);
        uint8_t* expected = malloc(size);
        EXPECT_TRUE(expected != NULL);
        EXPECT_EQ(fread(expected, 1, size, fp), size);
        fclose(fp);
        EXPECT_MEMEQ(data, expected, size);
        free(expected);
    }
    free(data);

    /* single-cluster files */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/hello.txt", &de), FAT_OK);
    data = &sentinel;
    size = 1;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_OK);
    EXPECT_EQ(size, 12);
    EXPECT_MEMEQ(data, "hello world\n", 12);
    free(data);

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "/dir1/hoge.txt", &de),
              FAT_OK);
    data = &sentinel;
    size = 1;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_OK);
    EXPECT_EQ(size, 11);
    EXPECT_MEMEQ(data, "I am hoge.\n", 11);
    free(data);

    /* empty file (size 0, first cluster 0): no buffer, no error.  The
     * fixture has no empty file, so the dirent is fabricated. */
    memset(&de, 0, sizeof(de));
    de.attributes = 0x20;
    de.first_cluster = 0;
    de.file_size = 0;
    data = &sentinel;
    size = 1;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_OK);
    EXPECT_EQ(data, NULL);
    EXPECT_EQ(size, 0);

    /* directories and volume labels are not readable as files */
    de.attributes = 0x10;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_ERR_INVALID_ARG);
    de.attributes = 0x08;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_ERR_INVALID_ARG);

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
    EXPECT_TRUE(a != b);

    EXPECT_EQ(iter_dir(a, FAT_CLUSTER_ROOT, count_root, &ca), FAT_OK);
    EXPECT_EQ(iter_dir(b, FAT_CLUSTER_ROOT, count_root, &cb), FAT_OK);
    EXPECT_TRUE(ca.labels == 1 && ca.files == 2 && ca.dirs == 2);
    EXPECT_TRUE(cb.labels == 1 && cb.files == 2 && cb.dirs == 2);

    /* closing one context must not disturb the other */
    fat_close(a);
    memset(&cb, 0, sizeof(cb));
    EXPECT_EQ(iter_dir(b, FAT_CLUSTER_ROOT, count_root, &cb), FAT_OK);
    EXPECT_TRUE(cb.labels == 1 && cb.files == 2 && cb.dirs == 2);
    fat_close(b);

    /* documented NULL handling */
    fat_close(NULL);
    EXPECT_EQ(fat_get_type(NULL), FT_UNKNOWN);
    EXPECT_EQ(fat_geometry(NULL), NULL);
}

/* ------------------------------------------------------------------ */
/* in-process mutated images (fat_open_mem)                            */
/* ------------------------------------------------------------------ */

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
    EXPECT_TRUE(entry_off != 0);
    static const uint8_t size_1m[4] = {0x00, 0x00, 0x10, 0x00}; /* LE 0x100000 */
    memcpy(img + entry_off + 28u, size_1m, 4);

    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img); /* the context owns its copy: using it past this free is the
                  point of the test */

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 0x100000);
    uint8_t* data = NULL;
    size_t n = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &n), FAT_ERR_BAD_CLUSTER);
    EXPECT_EQ(data, NULL); /* no buffer may escape on the error path */
    fat_close(ctx);
    ctx = NULL;

    /* (g) directory chain loop: FAT[11] = 11 makes dir2 point at itself.
     * Cluster 11 holds 32 fully-used entry slots (no 0x00 terminator), so
     * iteration must consult the FAT and trip the chain guard instead of
     * spinning forever. */
    img = copy_image(orig, size);
    set_fat12_entry(img, 11, 11);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    Dir2Counts counts = {0, 0, 0, 0};
    EXPECT_EQ(iter_dir(ctx, 11, count_dir2, &counts), FAT_ERR_BAD_CLUSTER);
    fat_close(ctx);
    ctx = NULL;

    /* fat_open_mem copies the image: the pristine buffer survives every
     * mutation round trip untouched */
    EXPECT_EQ(checksum(orig, size), sum_before);
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
    EXPECT_EQ(tm->tm_year, year);
    EXPECT_EQ(tm->tm_mon, mon);
    EXPECT_EQ(tm->tm_mday, mday);
    EXPECT_EQ(tm->tm_hour, hour);
    EXPECT_EQ(tm->tm_min, min);
    EXPECT_EQ(tm->tm_sec, sec);
    EXPECT_EQ(tm->tm_isdst, 0);
}

static void test_dos_date_to_tm(void)
{
    struct tm tm;

    /* DOS epoch: 1980-01-01 00:00:00 */
    memset(&tm, 0xAA, sizeof(tm));
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(1980, 1, 1),
                                 dos_time_bits(0, 0, 0), 0, &tm),
              FAT_OK);
    assert_tm_fields(&tm, 80, 0, 1, 0, 0, 0);

    /* max encodable timestamp: 2107-12-31 23:59:58 */
    memset(&tm, 0xAA, sizeof(tm));
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2107, 12, 31),
                                 dos_time_bits(23, 59, 58), 0, &tm),
              FAT_OK);
    assert_tm_fields(&tm, 207, 11, 31, 23, 59, 58);

    /* ordinary stamp with a 0.01s field: struct tm has no sub-second slot,
     * so the tenth must not disturb any decoded field.  fat.h only
     * promises the date/time decode ("pass 0 to ignore"), so this asserts
     * exactly that much: tenth=78 decodes identically to tenth=0. */
    memset(&tm, 0xAA, sizeof(tm));
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                                 dos_time_bits(12, 34, 56), 78, &tm),
              FAT_OK);
    assert_tm_fields(&tm, 126, 9, 5, 12, 34, 56);
    memset(&tm, 0xAA, sizeof(tm));
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                                 dos_time_bits(12, 34, 56), 0, &tm),
              FAT_OK);
    assert_tm_fields(&tm, 126, 9, 5, 12, 34, 56);

    /* out-of-range encoded fields (fat.h: month 0/13+, day 0, hour 24+) */
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 0, 5),
                                 dos_time_bits(12, 34, 56), 0, &tm),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 13, 5),
                                 dos_time_bits(12, 34, 56), 0, &tm),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 10, 0),
                                 dos_time_bits(12, 34, 56), 0, &tm),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                                 dos_time_bits(24, 0, 0), 0, &tm),
              FAT_ERR_INVALID_ARG);

    /* NULL out */
    EXPECT_EQ(fat_dos_date_to_tm(dos_date_bits(2026, 10, 5),
                                 dos_time_bits(12, 34, 56), 0, NULL),
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
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de), FAT_OK);

    uint8_t* whole = NULL;
    size_t whole_size = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &whole, &whole_size), FAT_OK);
    EXPECT_EQ(whole_size, de.file_size);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open(ctx, &de, &f), FAT_OK);
    EXPECT_TRUE(f != NULL);
    EXPECT_EQ(fat_file_size(f), whole_size);
    EXPECT_EQ(fat_file_tell(f), 0);

    /* whole file in a single read: full length delivered (a short read
     * only happens at EOF), bytes identical, cursor at the end */
    if (whole_size > 0) {
        uint8_t* buf = malloc(whole_size);
        EXPECT_TRUE(buf != NULL);
        size_t got = 0x5A5A;
        EXPECT_EQ(fat_file_read(f, buf, whole_size, &got), FAT_OK);
        EXPECT_EQ(got, whole_size);
        EXPECT_MEMEQ(buf, whole, whole_size);
        EXPECT_EQ(fat_file_tell(f), whole_size);
        /* at EOF further reads deliver 0 bytes with FAT_OK */
        got = 0x5A5A;
        EXPECT_EQ(fat_file_read(f, buf, 16, &got), FAT_OK);
        EXPECT_EQ(got, 0);
        free(buf);
    }

    /* 1-byte chunked walk over the first 2000 bytes (FAT12: crosses the
     * first cluster boundary at 1024); tell tracks every single step */
    {
        size_t steps = whole_size < 2000 ? whole_size : 2000;
        for (size_t i = 0; i < steps; i++) {
            uint8_t b = 0;
            size_t got = 0;
            EXPECT_EQ(fat_file_seek(f, i), FAT_OK);
            EXPECT_EQ(fat_file_tell(f), i);
            EXPECT_EQ(fat_file_read(f, &b, 1, &got), FAT_OK);
            EXPECT_EQ(got, 1);
            EXPECT_EQ(b, whole[i]);
            EXPECT_EQ(fat_file_tell(f), i + 1);
        }
    }

    /* seek edges: 0, EOF (legal), and one past the end */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    EXPECT_EQ(fat_file_tell(f), 0);
    EXPECT_EQ(fat_file_seek(f, whole_size), FAT_OK);
    EXPECT_EQ(fat_file_tell(f), whole_size);
    {
        uint8_t b = 0;
        size_t got = 0x5A5A;
        EXPECT_EQ(fat_file_read(f, &b, 1, &got), FAT_OK);
        EXPECT_EQ(got, 0);
    }
    EXPECT_EQ(fat_file_seek(f, whole_size + 1), FAT_ERR_INVALID_ARG);
    if (whole_size > 0) {
        /* short read ONLY at EOF: 2 bytes requested at size-1 deliver 1 */
        uint8_t b[2] = {0, 0};
        size_t got = 0x5A5A;
        EXPECT_EQ(fat_file_seek(f, whole_size - 1), FAT_OK);
        EXPECT_EQ(fat_file_read(f, b, 2, &got), FAT_OK);
        EXPECT_EQ(got, 1);
        EXPECT_EQ(b[0], whole[whole_size - 1]);
    }
    /* read spanning a cluster boundary: last byte of cluster 0 and first
     * byte of cluster 1 in one call */
    {
        uint32_t cs = fat_cluster_size(ctx);
        if (whole_size > cs) {
            uint8_t b[2] = {0, 0};
            size_t got = 0;
            EXPECT_EQ(fat_file_seek(f, cs - 1), FAT_OK);
            EXPECT_EQ(fat_file_read(f, b, 2, &got), FAT_OK);
            EXPECT_EQ(got, 2);
            EXPECT_TRUE(b[0] == whole[cs - 1] && b[1] == whole[cs]);
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
        EXPECT_EQ(fat_file_open(ctx, &de, &f), FAT_OK);
        EXPECT_EQ(fat_file_size(f), 0);
        EXPECT_EQ(fat_file_tell(f), 0);
        uint8_t b = 0;
        size_t got = 0x5A5A;
        EXPECT_EQ(fat_file_read(f, &b, 8, &got), FAT_OK);
        EXPECT_EQ(got, 0);
        EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
        EXPECT_EQ(fat_file_seek(f, 1), FAT_ERR_INVALID_ARG); /* past size 0 */
        fat_file_close(f);
    }

    /* non-file dirents are rejected, same rules as fat_read_file */
    {
        fat_dirent_t de;
        fat_file_t* f = NULL;
        memset(&de, 0, sizeof(de));
        de.attributes = 0x10; /* directory */
        de.first_cluster = 8;
        EXPECT_EQ(fat_file_open(ctx, &de, &f), FAT_ERR_INVALID_ARG);
        de.attributes = 0x08; /* volume label */
        EXPECT_EQ(fat_file_open(ctx, &de, &f), FAT_ERR_INVALID_ARG);
    }

    /* documented NULL handling */
    fat_file_close(NULL);
    EXPECT_EQ(fat_file_tell(NULL), 0);
    EXPECT_EQ(fat_file_size(NULL), 0);

    fat_close(ctx);
}

/* ------------------------------------------------------------------ */
/* B7: raw fat_dir_next stepping vs the iter_dir bridge loop           */
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

/* Stepping fat_dir_next by hand must enumerate exactly what the iter_dir
 * bridge collects on the same cluster -- same entries in the same order --
 * then report FAT_ERR_END_OF_DIR, stickily. (Since the API consolidation
 * both drive the same cursor; the comparison still pins the loop's
 * ordering and exhaustion contract.) */
static void assert_dir_cursor_matches_iter(fat_ctx_t* ctx, uint32_t cluster)
{
    EntryList list;
    memset(&list, 0, sizeof(list));
    EXPECT_EQ(iter_dir(ctx, cluster, collect_entries, &list), FAT_OK);
    EXPECT_EQ(list.overflow, 0);

    fat_dir_t* d = NULL;
    EXPECT_EQ(fat_dir_open(ctx, cluster, &d), FAT_OK);
    EXPECT_TRUE(d != NULL);
    for (int i = 0; i < list.n; i++) {
        const fat_dirent_t* e = NULL;
        const uint8_t* r = NULL;
        EXPECT_EQ(fat_dir_next(d, &e, &r), FAT_OK);
        EXPECT_TRUE(e != NULL && r != NULL);
        EXPECT_STR_EQ(e->name, list.e[i].name);
        EXPECT_EQ(e->attributes, list.e[i].attributes);
        EXPECT_EQ(e->first_cluster, list.e[i].first_cluster);
        EXPECT_EQ(e->file_size, list.e[i].file_size);
        /* raw32: the same 32 on-disk bytes, and their first 11 must be
         * the 8.3 encoding of the rendered name (dot entries are not 8.3
         * names, so those skip the round trip) */
        EXPECT_MEMEQ(r, list.raw[i], 32);
        if (e->name[0] != '.') {
            uint8_t name11[11];
            EXPECT_EQ(fat_name_to_83(e->name, name11), FAT_OK);
            EXPECT_MEMEQ(r, name11, 11);
        }
    }
    const fat_dirent_t* e = NULL;
    const uint8_t* r = NULL;
    EXPECT_EQ(fat_dir_next(d, &e, &r), FAT_ERR_END_OF_DIR);
    EXPECT_EQ(fat_dir_next(d, &e, &r), FAT_ERR_END_OF_DIR); /* sticky */
    fat_dir_close(d);
}

static void test_dir_cursor(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_dir_cursor_matches_iter(ctx, FAT_CLUSTER_ROOT);

    /* dir2: multi-cluster chain 11->43, 35 entries (2 dots + 33 subdirs) */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de), FAT_OK);
    assert_dir_cursor_matches_iter(ctx, de.first_cluster);

    /* a file's cluster is not a directory: hello.txt lives in cluster 2
     * (its data "hello world" does not start with a "." entry like every
     * real subdirectory does); cluster 1 is a reserved FAT entry */
    fat_dir_t* d = NULL;
    EXPECT_EQ(fat_dir_open(ctx, 2, &d), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_dir_open(ctx, 1, &d), FAT_ERR_INVALID_ARG);

    fat_dir_close(NULL); /* documented NULL safety */

    fat_close(ctx);
}

static void test_fat16_dir_cursor(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_dir_cursor_matches_iter(ctx, FAT_CLUSTER_ROOT);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
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
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
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
    (void)iter_dir(ctx, FAT_CLUSTER_ROOT, fuzz_noop_cb, NULL);
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
    EXPECT_TRUE(opened > 0);
    EXPECT_TRUE(rejected > 0);
    EXPECT_TRUE(read_ok > 0);

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
    EXPECT_TRUE(opened > 0);
    EXPECT_TRUE(rejected > 0);
    EXPECT_TRUE(read_ok > 0);

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
    /* Lead adjudication (§28.3, phase 10): NTRes-flagged 8.3 names now
     * render in lowercase */
    if (strcmp(entry->name, "hello.txt") == 0) {
        counts->saw_hello = 1;
        EXPECT_EQ(entry->first_cluster, 2);
        EXPECT_EQ(entry->file_size, 12);
    } else if (strcmp(entry->name, "test_5kb.txt") == 0) {
        counts->saw_5kb = 1;
        EXPECT_EQ(entry->first_cluster, 3);
        EXPECT_EQ(entry->file_size, 4962);
    } else if (strcmp(entry->name, "dir1") == 0) {
        counts->saw_dir1 = 1;
        EXPECT_EQ(entry->first_cluster, 13);
        EXPECT_TRUE(entry->attributes & 0x10);
    }
    if (entry->attributes & 0x10)
        counts->dirs++;
    else
        counts->files++;
}

static void test_fat16_open(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);

    EXPECT_EQ(fat_get_type(ctx), FT_FAT16);

    /* fatStart=1, rootDir=1+2*127=255, rootDirSectors=512*32/512=32,
     * dataStart=255+32=287, clusterCount=(32768-287)/1=32481 */
    const fat_geometry_t* g = fat_geometry(ctx);
    EXPECT_TRUE(g != NULL);
    EXPECT_EQ(g->bytes_per_sector, 512);
    EXPECT_EQ(g->sectors_per_cluster, 1);
    EXPECT_EQ(g->reserved_sectors, 1);
    EXPECT_EQ(g->fat_count, 2);
    EXPECT_EQ(g->fat_sectors, 127);
    EXPECT_EQ(g->root_entries, 512);
    EXPECT_EQ(g->total_sectors, 32768);
    EXPECT_EQ(g->fat_start_sector, 1);
    EXPECT_EQ(g->root_dir_sector, 255);
    EXPECT_EQ(g->root_dir_sectors, 32);
    EXPECT_EQ(g->root_cluster, 0); /* FAT12/16: fixed root region */
    EXPECT_EQ(g->data_start_sector, 287);
    EXPECT_EQ(g->cluster_count, 32481);
    EXPECT_TRUE(g->cluster_count >= 4085 && g->cluster_count < 65525);
    EXPECT_TRUE(g->fat_sectors > 0);

    EXPECT_EQ(fat_cluster_size(ctx), 512u);

    /* FSInfo is FAT32-only */
    fat_fsinfo_t fi;
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_ERR_UNSUPPORTED);

    fat_close(ctx);
}

static void test_fat16_fat_entries(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);

    /* media/reserved entries and the verified chains */
    EXPECT_EQ(fat_at(ctx, 0), 0xFFF8); /* media 0xF8 dirty pattern */
    EXPECT_EQ(fat_at(ctx, 1), 0xFFFF);
    EXPECT_EQ(fat_at(ctx, 2), 0xFFFF); /* hello.txt: single cluster, EOC */
    EXPECT_EQ(fat_at(ctx, 3), 4);     /* test_5kb.txt chain 3->4->..->12 */
    EXPECT_EQ(fat_at(ctx, 11), 12);
    EXPECT_EQ(fat_at(ctx, 12), 0xFFFF); /* EOC at chain end, 0xFFFF >= 0xFFF8 */
    EXPECT_EQ(fat_at(ctx, 13), 0xFFFF); /* dir1 */
    EXPECT_EQ(fat_at(ctx, 14), 0xFFFF); /* sub1 */
    EXPECT_EQ(fat_at(ctx, 15), 0xFFFF); /* page.txt */
    EXPECT_EQ(fat_at(ctx, 16), 0xFFFF); /* hoge.txt */

    /* indices 0..cluster_count+1 are readable, beyond that is an error */
    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t v = 0;
    EXPECT_EQ(fat_get_fat_entry(ctx, g->cluster_count + 1, &v), FAT_OK);
    EXPECT_EQ(fat_get_fat_entry(ctx, g->cluster_count + 2, &v),
              FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_fat16_root_iterate(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    Root16Counts counts = {0, 0, 0, 0, 0, 0, 0};

    EXPECT_EQ(iter_dir(ctx, FAT_CLUSTER_ROOT, count_root16, &counts),
              FAT_OK);
    /* DEMOF16 + hello.txt + test_5kb.txt + dir1 */
    EXPECT_EQ(counts.labels, 1);
    EXPECT_EQ(counts.files, 2);
    EXPECT_EQ(counts.dirs, 1);
    EXPECT_TRUE(counts.saw_label && counts.saw_hello && counts.saw_5kb);
    EXPECT_TRUE(counts.saw_dir1);

    fat_close(ctx);
}

static void test_fat16_lookup_read(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 13);
    EXPECT_TRUE(de.attributes & 0x10);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 14);

    /* multi-cluster chain: 4962 bytes over clusters 3->4->..->12 */
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert_read_string(ctx, "dir1/hoge.txt", "I am hoge.\n");
    assert_read_string(ctx, "dir1/sub1/page.txt", "You are page.\n");

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/noexist", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "noexist/foo", &de),
              FAT_ERR_PATH_NOT_FOUND);

    /* A2: synthetic root dirent on FAT16 too */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "..", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_TRUE(de.attributes & 0x10);

    fat_close(ctx);
}

/* A3: the FAT16 volume label never matches a lookup */
static void test_fat16_label_lookup(void)
{
    fat_ctx_t* ctx = open_image(IMG16_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "demof16", &de),
              FAT_ERR_NOT_FOUND);

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
    /* Lead adjudication (§28.3, phase 10): NTRes-flagged 8.3 names (files
     * 0x18, fillers 0x18, dirs 0x08) now render in lowercase */
    if (strcmp(entry->name, "hello.txt") == 0) {
        counts->saw_hello = 1;
        EXPECT_EQ(entry->first_cluster, 3);
        EXPECT_EQ(entry->file_size, 12);
    } else if (strcmp(entry->name, "test_5kb.txt") == 0) {
        counts->saw_5kb = 1;
        EXPECT_EQ(entry->first_cluster, 4);
        EXPECT_EQ(entry->file_size, 4962);
    } else if (strcmp(entry->name, "dir1") == 0) {
        counts->saw_dir1 = 1;
        EXPECT_EQ(entry->first_cluster, 56);
        EXPECT_TRUE(entry->attributes & 0x10);
    } else if (entry->name[0] == 'f' && strlen(entry->name) == 7) {
        /* fNN.txt filler; mtools allocated them in copy order, so the
         * Nth filler sits at cluster 14+N regardless of which root
         * cluster of the 2->54->55 chain its dirent landed in */
        EXPECT_TRUE(entry->name[1] >= '0' && entry->name[1] <= '9');
        EXPECT_TRUE(entry->name[2] >= '0' && entry->name[2] <= '9');
        int n = (entry->name[1] - '0') * 10 + (entry->name[2] - '0');
        EXPECT_TRUE(n <= 39);
        EXPECT_STR_EQ(entry->name + 3, ".txt");
        EXPECT_EQ(entry->first_cluster, 14u + (uint32_t)n);
        EXPECT_EQ(entry->file_size, (n < 10 ? 14u : 15u));
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

    EXPECT_EQ(fat_get_type(ctx), FT_FAT32);

    /* fatStart=32, dataStart=32+2*520=1072, rootDir region absent,
     * clusterCount=(67584-1072)/1=66512 >= 65525 */
    const fat_geometry_t* g = fat_geometry(ctx);
    EXPECT_TRUE(g != NULL);
    EXPECT_EQ(g->bytes_per_sector, 512);
    EXPECT_EQ(g->sectors_per_cluster, 1);
    EXPECT_EQ(g->reserved_sectors, 32);
    EXPECT_EQ(g->fat_count, 2);
    EXPECT_EQ(g->fat_sectors, 520); /* tableSize32 for FAT32 */
    EXPECT_EQ(g->root_entries, 0);
    EXPECT_EQ(g->total_sectors, 67584); /* totalSectors32 */
    EXPECT_EQ(g->fat_start_sector, 32);
    EXPECT_EQ(g->root_dir_sector, 0);
    EXPECT_EQ(g->root_dir_sectors, 0);
    EXPECT_EQ(g->root_cluster, 2); /* FAT32: root is a cluster chain */
    EXPECT_EQ(g->data_start_sector, 1072);
    EXPECT_EQ(g->cluster_count, 66512);
    EXPECT_TRUE(g->cluster_count >= 65525);

    EXPECT_EQ(fat_cluster_size(ctx), 512u);

    fat_close(ctx);
}

static void test_fat32_fat_entries(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);

    /* masked media/reserved entries and the verified chains */
    EXPECT_EQ(fat_at(ctx, 0), 0x0FFFFFF8); /* media 0xF8 pattern, masked */
    EXPECT_EQ(fat_at(ctx, 1), 0x0FFFFFFF); /* raw 0xFFFFFFFF masked */
    EXPECT_EQ(fat_at(ctx, 2), 54);         /* root chain 2->54->55 */
    EXPECT_EQ(fat_at(ctx, 3), 0x0FFFFFFF); /* hello.txt: single cluster */
    EXPECT_EQ(fat_at(ctx, 4), 5);          /* test_5kb chain 4->5->..->13 */
    EXPECT_EQ(fat_at(ctx, 12), 13);
    EXPECT_EQ(fat_at(ctx, 13), 0x0FFFFFFF);
    EXPECT_TRUE(fat_at(ctx, 13) >= 0x0FFFFFF8u); /* EOC at chain end */
    EXPECT_EQ(fat_at(ctx, 14), 0x0FFFFFFF);  /* F00.TXT */
    EXPECT_EQ(fat_at(ctx, 26), 0x0FFFFFFF);  /* F12.TXT */
    EXPECT_EQ(fat_at(ctx, 43), 0x0FFFFFFF);  /* F29.TXT */
    EXPECT_EQ(fat_at(ctx, 53), 0x0FFFFFFF);  /* F39.TXT */
    EXPECT_EQ(fat_at(ctx, 54), 55);          /* second root cluster */
    EXPECT_EQ(fat_at(ctx, 55), 0x0FFFFFFF);  /* root chain ends here */
    EXPECT_EQ(fat_at(ctx, 56), 0x0FFFFFFF);  /* dir1 */
    EXPECT_EQ(fat_at(ctx, 57), 0x0FFFFFFF);  /* sub1 */
    EXPECT_EQ(fat_at(ctx, 58), 0x0FFFFFFF);  /* page.txt */
    EXPECT_EQ(fat_at(ctx, 59), 0x0FFFFFFF);  /* hoge.txt */

    const fat_geometry_t* g = fat_geometry(ctx);
    uint32_t v = 0;
    EXPECT_EQ(fat_get_fat_entry(ctx, g->cluster_count + 1, &v), FAT_OK);
    EXPECT_EQ(fat_get_fat_entry(ctx, g->cluster_count + 2, &v),
              FAT_ERR_INVALID_ARG);

    fat_close(ctx);
}

static void test_fat32_root_iterate(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    Root32Counts counts = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    /* 44 entries spread over the 3-cluster root chain 2->54->55 */
    EXPECT_EQ(iter_dir(ctx, FAT_CLUSTER_ROOT, count_root32, &counts),
              FAT_OK);
    EXPECT_EQ(counts.labels, 1);
    EXPECT_EQ(counts.files, 42); /* hello + test_5kb + 40 fillers */
    EXPECT_EQ(counts.dirs, 1);   /* dir1 */
    EXPECT_EQ(counts.fillers, 40);
    EXPECT_EQ(counts.bad_name, 0);
    EXPECT_TRUE(counts.saw_label && counts.saw_hello && counts.saw_5kb);
    EXPECT_TRUE(counts.saw_dir1);

    fat_close(ctx);
}

static void test_fat32_lookup_read(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 56);
    EXPECT_TRUE(de.attributes & 0x10);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 57);

    /* multi-cluster chain: 4962 bytes over clusters 4->5->..->13 */
    assert_read_matches_host(ctx, "test_5kb.txt", "test_5kb.txt", 4962);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    assert_read_string(ctx, "dir1/hoge.txt", "I am hoge.\n");
    assert_read_string(ctx, "dir1/sub1/page.txt", "You are page.\n");
    /* filler reached through the multi-cluster root: F29.TXT lives in the
     * third root cluster (55) */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "f29.txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 43);
    EXPECT_EQ(de.file_size, 15);

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/noexist", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "noexist/foo", &de),
              FAT_ERR_PATH_NOT_FOUND);

    /* A2: synthetic root dirent; the FAT32 root is a real cluster chain
     * (2->54->55), but "."/".." there are still synthesized, not read */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, ".", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, FAT_CLUSTER_ROOT);
    EXPECT_TRUE(de.attributes & 0x10);

    fat_close(ctx);
}

/* A3: the FAT32 volume label never matches a lookup */
static void test_fat32_label_lookup(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "demof32", &de),
              FAT_ERR_NOT_FOUND);

    fat_close(ctx);
}

static void test_fat32_fsinfo(void)
{
    fat_ctx_t* ctx = open_image(IMG32_NAME);
    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));

    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    /* verified against mdir at fixture-creation time: 34024448 bytes free
     * = 66454 clusters * 512B exactly (66512 total - 58 in use) */
    EXPECT_EQ(fi.free_cluster_count, 66454);
    EXPECT_EQ(fi.next_free_cluster, 59);
    EXPECT_TRUE(fi.free_cluster_count != 0xFFFFFFFFu);

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
    EXPECT_EQ(fsinfo_sector, 1);
    memset(img + (size_t)fsinfo_sector * 512u, 0, 4);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 0xFFFFFFFFu);
    EXPECT_EQ(fi.next_free_cluster, 0xFFFFFFFFu);
    fat_close(ctx);
    ctx = NULL;

    /* (d) chain loop through a 32-bit FAT entry: TEST_5KB.TXT runs
     * 4->5->..->13; FAT[5] = 5 makes it point at itself, so the read must
     * trip the chain guard instead of spinning */
    img = copy_image(orig, size);
    set_fat32_entry(img, 5, 5);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de),
              FAT_OK);
    uint8_t* data = NULL;
    size_t n = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &n), FAT_ERR_BAD_CLUSTER);
    EXPECT_EQ(data, NULL); /* no buffer may escape on the error path */
    fat_close(ctx);
    ctx = NULL;

    /* fat_open_mem copies the image: the pristine buffer survives every
     * mutation round trip untouched */
    EXPECT_EQ(checksum(orig, size), sum_before);
    free(orig);
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
    EXPECT_EQ(fat_open_mem(img, size, &mctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(mctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de),
              FAT_OK);
    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open(mctx, &de, &f), FAT_OK);
    uint8_t* buf = malloc(de.file_size);
    EXPECT_TRUE(buf != NULL);
    size_t got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, buf, de.file_size, &got),
              FAT_ERR_BAD_CLUSTER);
    free(buf);
    fat_file_close(f);
    fat_close(mctx);
    free(orig);
}

void test_read_register(void)
{
    REGISTER(test_open);
    REGISTER(test_fat_entries);
    REGISTER(test_root_iterate);
    REGISTER(test_dir2_iterate);
    REGISTER(test_names);
    REGISTER(test_name_05_escape);
    REGISTER(test_nt_rendering);
    REGISTER(test_lookup);
    REGISTER(test_lookup_root_dots);
    REGISTER(test_lookup_volume_label);
    REGISTER(test_read_file);
    REGISTER(test_ctx_lifecycle);
    REGISTER(test_open_mem_errors);
    REGISTER(test_dos_date_to_tm);
    REGISTER(test_file_stream);
    REGISTER(test_dir_cursor);
    REGISTER(test_boot_fuzz_fat12);
    REGISTER_AS(test_fat16_open, NEED_FAT16);
    REGISTER_AS(test_fat16_fat_entries, NEED_FAT16);
    REGISTER_AS(test_fat16_root_iterate, NEED_FAT16);
    REGISTER_AS(test_fat16_lookup_read, NEED_FAT16);
    REGISTER_AS(test_fat16_label_lookup, NEED_FAT16);
    REGISTER_AS(test_fat16_file_stream, NEED_FAT16);
    REGISTER_AS(test_fat16_dir_cursor, NEED_FAT16);
    REGISTER_AS(test_boot_fuzz_values_fat16, NEED_FAT16);
    REGISTER_AS(test_fat32_open, NEED_FAT32);
    REGISTER_AS(test_fat32_fat_entries, NEED_FAT32);
    REGISTER_AS(test_fat32_root_iterate, NEED_FAT32);
    REGISTER_AS(test_fat32_lookup_read, NEED_FAT32);
    REGISTER_AS(test_fat32_label_lookup, NEED_FAT32);
    REGISTER_AS(test_fat32_fsinfo, NEED_FAT32);
    REGISTER_AS(test_open_mem_errors_fat32, NEED_FAT32);
    REGISTER_AS(test_fat32_file_stream, NEED_FAT32);
    REGISTER_AS(test_fat32_dir_cursor, NEED_FAT32);
    REGISTER_AS(test_boot_fuzz_values_fat32, NEED_FAT32);
}
