// test_oracle.c -- the mtools oracle tests: the library's view of an
// image must agree with what mdir/mtype/mcopy see, in both directions.

#include "test_util.h"

/* cursor-based name lookup (the LFN tests need LFN-aware find, which
 * fat_lookup does not do); returns 1 and fills *out when found */
static int dir_find(fat_ctx_t* ctx, uint32_t dir_cluster, const char* name,
                    fat_dirent_t* out)
{
    fat_dir_t* d = NULL;
    if (fat_dir_open(ctx, dir_cluster, &d) != FAT_OK)
        return 0;
    int found = 0;
    while (!found) {
        const fat_dirent_t* e = NULL;
        fat_result_t r = fat_dir_next(d, &e, NULL);
        EXPECT_TRUE(r == FAT_OK || r == FAT_ERR_END_OF_DIR);
        if (r != FAT_OK)
            break;
        if (strcmp(e->name, name) == 0) {
            *out = *e;
            found = 1;
        }
    }
    fat_dir_close(d);
    return found;
}

static void test_mtools_diff(void)
{
    fat_ctx_t* ctx = open_fixture();
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    assert_listing_matches_mdir(ctx, IMG_NAME, "", FAT_CLUSTER_ROOT);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    assert_listing_matches_mdir(ctx, IMG_NAME, "dir1", de.first_cluster);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2", &de), FAT_OK);
    assert_listing_matches_mdir(ctx, IMG_NAME, "dir2", de.first_cluster);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &de), FAT_OK);
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
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
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
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    assert_listing_matches_mdir(ctx, IMG32_NAME, "dir1", de.first_cluster);

    assert_mtype_matches_read(ctx, IMG32_NAME, "test_5kb.txt");

    fat_close(ctx);
}

/* 18.4 oracle: after our unlink, mdir -b agrees on the listing and
 * mtype still delivers a survivor byte-exact. */
static void test_mtools_unlink_oracle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt"), FAT_OK);

    EXPECT_EQ(fat_write(ctx, "/tmp/p6_orch12.fat"), FAT_OK);
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
    EXPECT_EQ(system(cmd), 0);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open("/tmp/p6_mdel.fat", &ctx), FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 4);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    /* our partial overwrite: 200 bytes at offset 1000 */
    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_seek(f, 1000), FAT_OK);
    uint8_t* patch = make_pattern(200);
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, patch, 200, &w), FAT_OK);
    EXPECT_EQ(w, 200);
    fat_file_close(f);
    free(patch);

    EXPECT_EQ(fat_write(ctx, "/tmp/p6_orcw.fat"), FAT_OK);
    assert_mtype_matches_read(ctx, "/tmp/p6_orcw.fat", "test_5kb.txt");
    fat_close(ctx);
    remove("/tmp/p6_orcw.fat");

    /* mtools' overwrite: mcopy -o the host file over hello.txt */
    write_image("/tmp/p6_orcw.fat", orig, size);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "mcopy -o -i /tmp/p6_orcw.fat test_5kb.txt ::hello.txt");
    EXPECT_EQ(system(cmd), 0);

    fat_ctx_t* fresh = NULL;
    EXPECT_EQ(fat_open("/tmp/p6_orcw.fat", &fresh), FAT_OK);
    assert_read_matches_host(fresh, "hello.txt", "test_5kb.txt", 4962);
    assert_mtype_matches_read(fresh, "/tmp/p6_orcw.fat", "hello.txt");
    fat_close(fresh);
    remove("/tmp/p6_orcw.fat");
    free(orig);
}

/* 20.7 mtools oracle: an LFN file mtools created must read back through
 * our APIs exactly as mdir/mtype see it */
static void test_mtools_lfn_read_oracle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    write_image("/tmp/p7_mcr.fat", orig, size);
    free(orig);
    FILE* fp = fopen("/tmp/p7_payload.txt", "wb");
    EXPECT_TRUE(fp != NULL);
    EXPECT_EQ(fwrite("mtools lfn oracle\n", 1, 18, fp), 18);
    fclose(fp);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "mcopy -i /tmp/p7_mcr.fat /tmp/p7_payload.txt "
             "::\"mtools wrote this long name.txt\"");
    EXPECT_EQ(system(cmd), 0);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open("/tmp/p7_mcr.fat", &ctx), FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT,
                         "mtools wrote this long name.txt", &de));
    EXPECT_EQ(de.file_size, 18);
    uint8_t* buf = NULL;
    size_t n = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &buf, &n), FAT_OK);
    EXPECT_TRUE(n == 18 && memcmp(buf, "mtools lfn oracle\n", 18) == 0);
    free(buf);
    assert_mtype_matches_read(ctx, "/tmp/p7_mcr.fat",
                              "mtools wrote this long name.txt");
    fat_close(ctx);

    remove("/tmp/p7_mcr.fat");
    remove("/tmp/p7_payload.txt");
}

/* 20.7 mtools oracle, write direction: our LFN file must be listed by
 * mdir and typed by mtype (both by LFN and by alias) */
static void test_mtools_lfn_write_oracle(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt",
                             (const uint8_t*)"hello lfn data", 14, NULL),
              FAT_OK);
    EXPECT_EQ(fat_write(ctx, "/tmp/p7_mcw.fat"), FAT_OK);
    fat_close(ctx);

    /* mdir -b lists the LFN (probed: -b prints the long name) */
    MDirEntry entries[MDIR_MAX];
    int n = run_mdir_b("/tmp/p7_mcw.fat", "", entries, MDIR_MAX);
    EXPECT_TRUE(n >= 0);
    int seen = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(entries[i].name, "longnames.txt") == 0)
            seen = 1;
    EXPECT_TRUE(seen);

    fat_ctx_t* back = NULL;
    EXPECT_EQ(fat_open("/tmp/p7_mcw.fat", &back), FAT_OK);
    assert_mtype_matches_read(back, "/tmp/p7_mcw.fat", "longnames.txt");
    assert_mtype_matches_read(back, "/tmp/p7_mcw.fat", "LONGNA~1.TXT");
    fat_close(back);
    remove("/tmp/p7_mcw.fat");
}

void test_oracle_register(void)
{
    REGISTER_AS(test_mtools_diff, NEED_MTOOLS);
    REGISTER_AS(test_fat16_mtools_diff, NEED_FAT16 | NEED_MTOOLS);
    REGISTER_AS(test_fat32_mtools_diff, NEED_FAT32 | NEED_MTOOLS);
    REGISTER_AS(test_mtools_unlink_oracle, NEED_MTOOLS);
    REGISTER_AS(test_mtools_mdel_oracle, NEED_MTOOLS);
    REGISTER_AS(test_mtools_overwrite_oracle, NEED_MTOOLS);
    REGISTER_AS(test_mtools_lfn_read_oracle, NEED_MTOOLS);
    REGISTER_AS(test_mtools_lfn_write_oracle, NEED_MTOOLS);
}
