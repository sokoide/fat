/* tests/test_write.c -- the write-side half of the old monolithic
 * testmain.c: FAT entry writers, cluster alloc/free, dirent creation,
 * fat_write_file/fat_write, unlink/rmdir and the write cursors
 * (truncate/write), on FAT12 plus the FAT16/FAT32 fixtures where noted.
 * Shared helpers and the EXPECT framework come from test_util.h; the
 * register function at the bottom is called by test_main.c. */

#include "test_util.h"

/* ------------------------------------------------------------------ */
/* file-local helpers                                                  */
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

/* fixture byte boundaries (demof12, see test_open): the FAT tables
 * occupy sectors 1..6 ([512, 3584)), the fixed root region sectors
 * 7..13 ([3584, 7168)), the data area starts at sector 14 */
#define F12_ROOT_BYTES (FIXTURE_ROOT_SECTOR * 512u)

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
    EXPECT_EQ(fat_name_to_83(name, want11), FAT_OK);
    fat_dir_t* d = NULL;
    EXPECT_EQ(fat_dir_open(ctx, dir_cluster, &d), FAT_OK);
    int found = 0;
    while (!found) {
        const fat_dirent_t* e = NULL;
        const uint8_t* r = NULL;
        fat_result_t rr = fat_dir_next(d, &e, &r);
        EXPECT_TRUE(rr == FAT_OK || rr == FAT_ERR_END_OF_DIR);
        if (rr != FAT_OK)
            break;
        if (e->name[0] == '.')
            continue; /* dot entries are not 8.3 names */
        uint8_t got11[11];
        EXPECT_EQ(fat_name_to_83(e->name, got11), FAT_OK);
        if (memcmp(got11, want11, 11) == 0) {
            memcpy(out, r, 32);
            found = 1;
        }
    }
    fat_dir_close(d);
    EXPECT_TRUE(found);
}

/* slot integrity (18.2): only the first-cluster (bytes 20-21, 26-27)
 * and file-size (28-31) fields may differ -- name, attributes and every
 * timestamp stay byte-identical (truncate/write touch no timestamps). */
static void assert_slot_only_size_cluster_changed(const uint8_t before[32],
                                                  const uint8_t after[32],
                                                  uint32_t want_cluster,
                                                  uint32_t want_size)
{
    EXPECT_MEMEQ(before, after, 20);          /* name..lstAccDate */
    EXPECT_MEMEQ(before + 22, after + 22, 4); /* wrtTime/wrtDate */
    uint32_t cluster = ((uint32_t)after[20] << 16) |
                       ((uint32_t)after[21] << 24) |
                       (uint32_t)after[26] |
                       ((uint32_t)after[27] << 8);
    uint32_t size = (uint32_t)after[28] | ((uint32_t)after[29] << 8) |
                    ((uint32_t)after[30] << 16) | ((uint32_t)after[31] << 24);
    EXPECT_EQ(cluster, want_cluster);
    EXPECT_EQ(size, want_size);
}

/* first `n` bytes of the committed host fixture file */
static void read_host_prefix(size_t n, uint8_t* buf)
{
    FILE* fp = fopen("test_5kb.txt", "rb");
    if (fp == NULL)
        perror("test_5kb.txt");
    EXPECT_TRUE(fp != NULL);
    EXPECT_EQ(fread(buf, 1, n, fp), n);
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

/* ------------------------------------------------------------------ */
/* phase 4: write support (fat_set_fat_entry, fat_alloc_cluster,       */
/* fat_free_chain, fat_add_dirent, fat_write_file, fat_write)          */
/* ------------------------------------------------------------------ */

/* 12.1: FAT12 nibble neighbours survive the read-modify-write, both FAT
 * copies receive the update, and the argument/value ranges hold. */
static void test_set_fat_entry12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    /* 3 (even) and 4 (odd) sit inside the test_5kb chain; 9/10 are a free
     * even/odd pair.  The written entries change, every neighbour keeps
     * its original relationship. */
    EXPECT_EQ(fat_set_fat_entry(ctx, 3, 0x123), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, 4, 0x456), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, 9, 0x0AA), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, 10, 0x0BB), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 2), 0xFFF);
    EXPECT_EQ(fat_at(ctx, 3), 0x123);
    EXPECT_EQ(fat_at(ctx, 4), 0x456);
    EXPECT_EQ(fat_at(ctx, 5), 6);
    EXPECT_EQ(fat_at(ctx, 6), 7);
    EXPECT_EQ(fat_at(ctx, 7), 0xFFF);
    EXPECT_EQ(fat_at(ctx, 8), 0xFFF);
    EXPECT_EQ(fat_at(ctx, 9), 0x0AA);
    EXPECT_EQ(fat_at(ctx, 10), 0x0BB);
    EXPECT_EQ(fat_at(ctx, 11), 43);

    /* FREE and EOC are ordinary writable values */
    EXPECT_EQ(fat_set_fat_entry(ctx, 9, 0), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 9), 0);
    EXPECT_EQ(fat_set_fat_entry(ctx, 9, 0xFFF), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 9), 0xFFF);

    /* cluster range: 2..cluster_count+1 */
    EXPECT_EQ(fat_set_fat_entry(NULL, 3, 1), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(ctx, 0, 1), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(ctx, 1, 1), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(ctx, g->cluster_count + 1, 1), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, g->cluster_count + 2, 1),
              FAT_ERR_INVALID_ARG);
    /* value range: 12-bit entries cap at 0xFFF */
    EXPECT_EQ(fat_set_fat_entry(ctx, 3, 0x1000), FAT_ERR_INVALID_ARG);

    /* both FAT copies: flush and compare the raw tables byte for byte
     * (they start identical, so mirrored writes keep them identical) and
     * decode the touched entries by hand in each table */
    EXPECT_EQ(fat_write(ctx, "/tmp/p4_set12.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_set12.fat", &fn);
    EXPECT_EQ(fn, size);
    EXPECT_MEMEQ(back + FIXTURE_FAT_SECTOR * 512u, back + 4u * 512u,
                 3u * 512u);
    for (unsigned t = 0; t < 2u; t++) {
        unsigned base = t == 0u ? FIXTURE_FAT_SECTOR : 4u;
        EXPECT_EQ(raw_fat12_at(back, base, 2), 0xFFF);
        EXPECT_EQ(raw_fat12_at(back, base, 3), 0x123);
        EXPECT_EQ(raw_fat12_at(back, base, 4), 0x456);
        EXPECT_EQ(raw_fat12_at(back, base, 5), 6);
        EXPECT_EQ(raw_fat12_at(back, base, 6), 7);
        EXPECT_EQ(raw_fat12_at(back, base, 7), 0xFFF);
        EXPECT_EQ(raw_fat12_at(back, base, 9), 0xFFF);
        EXPECT_EQ(raw_fat12_at(back, base, 10), 0x0BB);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint32_t c1 = 0;
    uint32_t c2 = 0;
    EXPECT_EQ(fat_alloc_cluster(ctx, &c1), FAT_OK);
    EXPECT_TRUE(c1 >= 2u && c1 <= fat_geometry(ctx)->cluster_count + 1u);
    EXPECT_EQ(raw_fat12_at(orig, FIXTURE_FAT_SECTOR, c1), 0); /* was free */
    EXPECT_EQ(fat_at(ctx, c1), 0xFFF); /* now EOC, exact constant */
    EXPECT_EQ(fat_alloc_cluster(ctx, &c2), FAT_OK);
    EXPECT_TRUE(c2 != c1);
    EXPECT_EQ(raw_fat12_at(orig, FIXTURE_FAT_SECTOR, c2), 0);
    EXPECT_EQ(fat_at(ctx, c2), 0xFFF);
    EXPECT_EQ(fat_at(ctx, c1), 0xFFF); /* the second alloc changed nothing */

    EXPECT_EQ(fat_alloc_cluster(NULL, &c1), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_alloc_cluster(ctx, NULL), FAT_ERR_INVALID_ARG);
    fat_close(ctx);

    /* DISK_FULL: every data entry nonzero leaves nothing to allocate */
    img = copy_image(orig, size);
    for (uint32_t c = 2u; c <= 713u + 1u; c++)
        if (raw_fat12_at(img, FIXTURE_FAT_SECTOR, c) == 0)
            set_fat12_entry(img, c, 0x001);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_alloc_cluster(ctx, &c1), FAT_ERR_DISK_FULL);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_free_chain(ctx, 3), FAT_OK);
    for (uint32_t c = 3u; c <= 7u; c++)
        EXPECT_EQ(fat_at(ctx, c), 0);
    /* the scan restarts at the lowest cluster, so 3 comes back first */
    uint32_t c = 0;
    EXPECT_EQ(fat_alloc_cluster(ctx, &c), FAT_OK);
    EXPECT_EQ(c, 3);

    /* single-cluster chain */
    EXPECT_EQ(fat_free_chain(ctx, 2), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 2), 0);
    fat_close(ctx);

    /* looping chain: FAT[8] = 8 */
    img = copy_image(orig, size);
    set_fat12_entry(img, 8, 8);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_free_chain(ctx, 8), FAT_ERR_BAD_CLUSTER);
    EXPECT_EQ(fat_at(ctx, 8), 0); /* the walked entry was still freed */

    EXPECT_EQ(fat_free_chain(NULL, 3), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_free_chain(ctx, 0), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_free_chain(ctx, 1), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_free_chain(ctx, fat_geometry(ctx)->cluster_count + 2),
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
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

    EXPECT_EQ(fat_add_dirent(ctx, 8, "newfile.txt", &tmpl), FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/newfile.txt", &de),
              FAT_OK);
    EXPECT_STR_EQ(de.name, "NEWFILE.TXT");
    EXPECT_EQ(de.attributes, 0x20);
    EXPECT_EQ(de.creation_time_tenth, 78);
    EXPECT_EQ(de.creation_time, dos_time_bits(12, 34, 56));
    EXPECT_EQ(de.creation_date, dos_date_bits(2026, 10, 5));
    EXPECT_EQ(de.last_access_date, dos_date_bits(2026, 10, 6));
    EXPECT_EQ(de.last_write_time, dos_time_bits(21, 5, 4));
    EXPECT_EQ(de.last_write_date, dos_date_bits(2025, 12, 31));
    EXPECT_EQ(de.first_cluster, 99);
    EXPECT_EQ(de.file_size, 123);

    /* timestamps decode through the public helper */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    EXPECT_EQ(fat_dos_date_to_tm(de.creation_date, de.creation_time,
                                 de.creation_time_tenth, &tm),
              FAT_OK);
    assert_tm_fields(&tm, 126, 9, 5, 12, 34, 56);

    /* dir1: 5 live entries before, 6 after */
    EXPECT_EQ(count_dir_entries(ctx, 8), 6);

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
    EXPECT_TRUE(hoge_off != 0);
    img[hoge_off] = 0xE5;

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    tmpl.first_cluster = 7;
    tmpl.file_size = 5;
    EXPECT_EQ(fat_add_dirent(ctx, 8, "hoga.txt", &tmpl), FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoga.txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 7);
    EXPECT_EQ(de.file_size, 5);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de),
              FAT_ERR_NOT_FOUND);

    /* raw slot check on the flushed image: HOGA sits exactly where HOGE
     * was, exactly once, and HOGE is gone */
    EXPECT_EQ(fat_write(ctx, "/tmp/p4_reuse.fat"), FAT_OK);
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
    EXPECT_EQ(hoga_count, 1);
    EXPECT_EQ(hoga_off, hoge_off);
    EXPECT_EQ(hoge_count, 0);
    free(back);
    remove("/tmp/p4_reuse.fat");

    /* hoga replaced hoge: still 5 live entries */
    EXPECT_EQ(count_dir_entries(ctx, 8), 5);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "hello.txt", &tmpl),
              FAT_ERR_EXISTS);
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5);
    fat_close(ctx);

    /* NAME_TOO_LONG and NULL arguments. Lead adjudication 2026-10-06
     * (phase 7): "toolongname.txt" is a legal LFN now, so the error path
     * is exercised with a name past the 255-UTF-8-byte LFN limit instead */
    img = copy_image(orig, size);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    char toolong[300];
    memset(toolong, 'a', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    EXPECT_EQ(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, toolong, &tmpl),
              FAT_ERR_NAME_TOO_LONG);
    EXPECT_EQ(fat_add_dirent(NULL, FAT_CLUSTER_ROOT, "x", &tmpl),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, NULL, &tmpl),
              FAT_ERR_INVALID_ARG);
    fat_close(ctx);

    /* DIR_FULL: rootEntryCount = 5 = exactly the live root entries
     * (label + hello + test_5kb + dir1 + dir2, slots 0..4), so the fixed
     * FAT12 root region has no free slot and cannot extend */
    img = copy_image(orig, size);
    static const uint8_t re5[2] = {0x05, 0x00};
    memcpy(img + BPB_ROOT_ENTRIES, re5, 2);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "extra.txt", &tmpl),
              FAT_ERR_DIR_FULL);
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
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

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             &tmpl),
              FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n, 5);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "new.txt", &de), FAT_OK);
    EXPECT_EQ(de.attributes, 0x20);
    EXPECT_EQ(de.creation_date, dos_date_bits(2026, 2, 3));
    EXPECT_EQ(de.creation_time, dos_time_bits(9, 8, 6));
    EXPECT_EQ(de.last_write_date, dos_date_bits(2026, 2, 3));
    EXPECT_EQ(de.last_write_time, dos_time_bits(9, 8, 6));

    /* attributes arrive verbatim from tmpl (RO|archive; hidden/system
     * would vanish from mdir's default listing and break the oracle) */
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x21;
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "attr.txt",
                             (const uint8_t*)"xyz", 3, &tmpl),
              FAT_OK);
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "attr.txt", &de), FAT_OK);
    EXPECT_EQ(de.attributes, 0x21);

    /* pre-existing content untouched */
    assert_read_string(ctx, "hello.txt", "hello world\n");

    /* mtools oracle on the flushed image */
    EXPECT_EQ(fat_write(ctx, "/tmp/p4_rt12.fat"), FAT_OK);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    static const size_t sizes[] = {1023, 1024, 1025, 2048, 2049};
    static const uint32_t clusters[] = {1, 1, 2, 2, 3};
    char name[16];
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "b%zu.txt", sizes[i]);
        uint8_t* data = make_pattern(sizes[i]);
        EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, name, data, sizes[i],
                                 NULL),
                  FAT_OK);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    /* 662 free data clusters: 713 total minus the 51 in use (every
     * cluster 2..52 holds hello/test_5kb/dir1/dir2 and the 33+2 empty
     * subdirectories spread under dir1 and dir2; FAT-verified) */
    uint32_t free_before = count_free_clusters(ctx);
    EXPECT_EQ(free_before, 662u);

    /* empty file: data NULL is only legal with size 0 */
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "empty.txt", NULL, 0,
                             NULL),
              FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "empty.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 0);
    EXPECT_EQ(de.first_cluster, 0);
    /* NULL tmpl = ATTR_ARCHIVE and zero timestamps */
    EXPECT_EQ(de.attributes, 0x20);
    EXPECT_EQ(de.creation_time_tenth, 0);
    EXPECT_EQ(de.creation_time, 0);
    EXPECT_EQ(de.creation_date, 0);
    EXPECT_EQ(de.last_access_date, 0);
    EXPECT_EQ(de.last_write_time, 0);
    EXPECT_EQ(de.last_write_date, 0);
    uint8_t* back = NULL;
    size_t bn = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &back, &bn), FAT_OK);
    EXPECT_TRUE(back == NULL);
    EXPECT_EQ(bn, 0);

    /* empty wrote no cluster */
    EXPECT_EQ(count_free_clusters(ctx), free_before);

    /* EXISTS: the original survives byte for byte */
    uint8_t one = 'x';
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "hello.txt", &one, 1,
                             NULL),
              FAT_ERR_EXISTS);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    EXPECT_EQ(count_free_clusters(ctx), free_before); /* nothing leaked */

    /* data NULL with size > 0 */
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "null.dat", NULL, 10,
                             NULL),
              FAT_ERR_INVALID_ARG);
    /* NULL ctx / NULL name / unrepresentable name */
    EXPECT_EQ(fat_write_file(NULL, FAT_CLUSTER_ROOT, "x", NULL, 0, NULL),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, NULL, NULL, 0, NULL),
              FAT_ERR_INVALID_ARG);
    /* Lead adjudication 2026-10-06 (phase 7): "toolongname.txt" is a legal
     * LFN now; use a name past the 255-UTF-8-byte LFN limit instead */
    char toolong[300];
    memset(toolong, 'a', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, toolong, NULL, 0, NULL),
              FAT_ERR_NAME_TOO_LONG);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    size_t n = 5000; /* needs 5 clusters, only 2 are free */
    uint8_t* data = make_pattern(n);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "big.txt", data, n,
                             NULL),
              FAT_ERR_DISK_FULL);
    free(data);

    /* the two clusters were rolled back to FREE, nothing else leaked */
    EXPECT_EQ(fat_at(ctx, 300), 0);
    EXPECT_EQ(fat_at(ctx, 301), 0);
    EXPECT_EQ(count_free_clusters(ctx), 2u);
    /* no dirent appeared, the originals are intact */
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "big.txt", &de),
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dirent_t dir;
    memset(&dir, 0, sizeof(dir));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &dir), FAT_OK);
    EXPECT_EQ(dir.first_cluster, 12);

    size_t n = 100;
    uint8_t* data = make_pattern(n);
    EXPECT_EQ(fat_write_file(ctx, dir.first_cluster, "new.txt", data, n,
                             NULL),
              FAT_OK);
    /* reachable through the full path; 100 bytes fit one 1024B cluster */
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/new.txt",
                           data, n, 1);
    /* subdir1 had ".", "..", page.txt, test_5kb.txt; now 5 entries */
    EXPECT_EQ(count_dir_entries(ctx, dir.first_cluster), 5);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    size_t n = 5000;
    uint8_t* data = make_pattern(n);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);

    EXPECT_EQ(fat_write(NULL, "/tmp/p4_flush12.fat"), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_write(ctx, NULL), FAT_ERR_INVALID_ARG);

    EXPECT_EQ(fat_write(ctx, "/tmp/p4_flush12.fat"), FAT_OK);

    /* exactly the whole image, and it reopens clean */
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_flush12.fat", &fn);
    EXPECT_EQ(fn, size);
    free(back);
    fat_ctx_t* fresh = NULL;
    EXPECT_EQ(fat_open("/tmp/p4_flush12.fat", &fresh), FAT_OK);
    EXPECT_EQ(fat_get_type(fresh), FT_FAT12);
    assert_same_geometry(fat_geometry(ctx), fat_geometry(fresh));

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(fresh, FAT_CLUSTER_ROOT, "new.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, n);
    uint8_t* rdata = NULL;
    size_t rn = 0;
    EXPECT_EQ(fat_read_file(fresh, &de, &rdata, &rn), FAT_OK);
    EXPECT_EQ(rn, n);
    EXPECT_MEMEQ(rdata, data, n);
    free(rdata);
    assert_read_string(fresh, "hello.txt", "hello world\n");
    fat_close(fresh);

    /* an unwritable destination reports FAT_ERR_IO */
    EXPECT_EQ(fat_write(ctx, "/tmp/no_such_dir_p4/x.fat"), FAT_ERR_IO);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    uint32_t before = count_free_clusters(ctx);

    /* set/get roundtrip and the 16-bit value ceiling */
    EXPECT_EQ(fat_at(ctx, 20), 0); /* free cluster past the fixture chains */
    EXPECT_EQ(fat_set_fat_entry(ctx, 20, 0x1234), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 20), 0x1234);
    EXPECT_EQ(fat_set_fat_entry(ctx, 21, 0xFFFF), FAT_OK); /* EOC */
    EXPECT_EQ(fat_at(ctx, 21), 0xFFFF);
    EXPECT_EQ(fat_set_fat_entry(ctx, 22, 0x10000), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(ctx, 1, 1), FAT_ERR_INVALID_ARG);

    /* alloc then free restores the free count exactly */
    uint32_t c = 0;
    EXPECT_EQ(fat_alloc_cluster(ctx, &c), FAT_OK);
    EXPECT_EQ(fat_at(ctx, c), 0xFFFF);
    EXPECT_EQ(fat_set_fat_entry(ctx, 20, 0), FAT_OK); /* restore */
    EXPECT_EQ(fat_set_fat_entry(ctx, 21, 0), FAT_OK);
    EXPECT_EQ(fat_free_chain(ctx, c), FAT_OK);
    EXPECT_EQ(count_free_clusters(ctx), before);

    /* 1000 bytes over 2 clusters of 512B */
    size_t n = 1000;
    uint8_t* data = make_pattern(n);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    uint32_t before9 = fat_at(ctx, 9);
    uint32_t before11 = fat_at(ctx, 11);
    EXPECT_EQ(fat_set_fat_entry(ctx, 10, 0x01234567u), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 10), 0x01234567u); /* masked readback */
    EXPECT_EQ(fat_at(ctx, 9), before9);
    EXPECT_EQ(fat_at(ctx, 11), before11);

    EXPECT_EQ(fat_set_fat_entry(ctx, 10, 0x10000000u), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(ctx, 0, 1), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(ctx, g->cluster_count + 2, 1),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_set_fat_entry(NULL, 10, 1), FAT_ERR_INVALID_ARG);

    /* raw check: reserved nibble kept in both FAT copies after flush */
    EXPECT_EQ(fat_write(ctx, "/tmp/p4_set32.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_set32.fat", &fn);
    EXPECT_EQ(fn, size);
    for (unsigned t = 0; t < 2u; t++) {
        size_t off =
            (size_t)(F32_FAT_SECTOR + t * 520u) * 512u + 10u * 4u;
        uint32_t v = (uint32_t)back[off] |
                     ((uint32_t)back[off + 1] << 8) |
                     ((uint32_t)back[off + 2] << 16) |
                     ((uint32_t)back[off + 3] << 24);
        EXPECT_EQ(v, 0xA1234567u); /* 0xA<<28 | 0x01234567 */
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    uint32_t free_before = fi.free_cluster_count;
    EXPECT_EQ(free_before, 66454u); /* fixture fact, mdir cross-checked */

    uint32_t c = 0;
    EXPECT_EQ(fat_alloc_cluster(ctx, &c), FAT_OK);
    EXPECT_EQ(fat_at(ctx, c), 0x0FFFFFFFu); /* EOC, exact constant */
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, free_before - 1u);
    EXPECT_TRUE(fi.next_free_cluster >= 2u);
    EXPECT_TRUE(fi.next_free_cluster <= g->cluster_count + 1u);
    EXPECT_TRUE(fi.next_free_cluster != 0xFFFFFFFFu);

    /* freeing it back restores the count (hint head update is sane) */
    EXPECT_EQ(fat_free_chain(ctx, c), FAT_OK);
    EXPECT_EQ(fat_at(ctx, c), 0);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, free_before);
    EXPECT_TRUE(fi.next_free_cluster >= 2u);
    EXPECT_TRUE(fi.next_free_cluster <= g->cluster_count + 1u);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 44);
    EXPECT_EQ(fat_at(ctx, 55), 0x0FFFFFFFu); /* chain ends at 55 */

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    for (int i = 1; i <= 5; i++) {
        char name[16];
        snprintf(name, sizeof(name), "x%d.txt", i);
        EXPECT_EQ(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, name, &tmpl), FAT_OK);
    }

    /* the chain grew: 55 now links to a fresh EOC cluster */
    uint32_t next = fat_at(ctx, 55);
    EXPECT_TRUE(next >= 2u && next <= g->cluster_count + 1u);
    EXPECT_EQ(fat_at(ctx, next), 0x0FFFFFFFu);

    /* 44 + 5 live entries */
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 49);

    /* dirent-only adds allocate exactly the one extension cluster */
    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u - 1u);

    /* the new cluster is zeroed except for the single fresh entry (x5,
     * the 5th add, is the one that forced the extension) */
    EXPECT_EQ(fat_write(ctx, "/tmp/p4_ext32.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p4_ext32.fat", &fn);
    size_t cl_bytes = (size_t)g->sectors_per_cluster * g->bytes_per_sector;
    size_t cl_off = ((size_t)g->data_start_sector + (next - 2u) *
                    g->sectors_per_cluster) * g->bytes_per_sector;
    EXPECT_MEMEQ(back + cl_off, "X5      TXT", 11);
    for (size_t i = 32; i < cl_bytes; i++)
        EXPECT_EQ(back[cl_off + i], 0);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);

    /* 5000 bytes over 10 clusters of 512B */
    size_t n = 5000;
    uint8_t* data = make_pattern(n);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n, 10);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u - 10u);
    EXPECT_TRUE(fi.next_free_cluster >= 2u);
    EXPECT_TRUE(fi.next_free_cluster <= g->cluster_count + 1u);

    /* empty file allocates nothing */
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "empty.txt", NULL, 0,
                             NULL),
              FAT_OK);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u - 10u);

    /* write into dir1 (cluster 56), one more cluster */
    fat_dirent_t dir;
    memset(&dir, 0, sizeof(dir));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &dir), FAT_OK);
    size_t sn = 100;
    uint8_t* sdata = make_pattern(sn);
    EXPECT_EQ(fat_write_file(ctx, dir.first_cluster, "sub.txt", sdata, sn,
                             NULL),
              FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "dir1/sub.txt", sdata, sn,
                           1);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u - 11u);

    /* originals intact + mtools oracle on the flushed image */
    assert_read_string(ctx, "hello.txt", "hello world\n");
    EXPECT_EQ(fat_write(ctx, "/tmp/p4_rt32.fat"), FAT_OK);
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
/* phase 6: deletion + write cursors (fat_unlink, fat_rmdir,           */
/* fat_file_open_write, fat_file_truncate, fat_file_write)             */
/* ------------------------------------------------------------------ */

/* 18.1: unlink marks the slot 0xE5, frees the chain (FAT entries 0,
 * free count back), leaves every neighbour byte-identical, and works
 * in subdirectories too (hoge.txt in dir1). */
static void test_unlink_basic12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint32_t free_before = count_free_clusters(ctx);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de), FAT_OK);
    uint32_t hoge_cluster = de.first_cluster;
    EXPECT_TRUE(hoge_cluster >= 2u);

    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, 2), 0); /* hello's single cluster is free again */
    EXPECT_EQ(count_free_clusters(ctx), free_before + 1u);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 4);

    /* same story inside dir1: hoge.txt (single cluster) */
    EXPECT_EQ(fat_unlink(ctx, 8, "hoge.txt"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/hoge.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, hoge_cluster), 0);
    EXPECT_EQ(count_free_clusters(ctx), free_before + 2u);
    EXPECT_EQ(count_dir_entries(ctx, 8), 4); /* . .. subdir1 subdir2 */

    /* raw bytes on the flushed image: both slots now start with 0xE5,
     * every other root slot is byte-identical */
    EXPECT_EQ(fat_write(ctx, "/tmp/p6_unlink12.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_unlink12.fat", &fn);
    EXPECT_EQ(fn, size);
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
    EXPECT_TRUE(hello_off && label_off && kb_off && dir1_off && dir2_off);
    EXPECT_TRUE(hoge_off != 0);
    EXPECT_EQ(back[hello_off], 0xE5);
    EXPECT_EQ(back[hoge_off], 0xE5);
    EXPECT_MEMEQ(back + label_off, orig + label_off, 32);
    EXPECT_MEMEQ(back + kb_off, orig + kb_off, 32);
    EXPECT_MEMEQ(back + dir1_off, orig + dir1_off, 32);
    EXPECT_MEMEQ(back + dir2_off, orig + dir2_off, 32);
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
    EXPECT_TRUE(hello_off != 0);

    fat_io_t* io = NULL;
    EXPECT_EQ(fat_io_file(&io, "/tmp/p6_sync.fat"), FAT_OK);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_io(io, &ctx), FAT_OK);
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt"), FAT_OK);
    EXPECT_EQ(fat_sync(ctx), FAT_OK);
    fat_close(ctx);

    /* the deletion reached the file itself */
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_sync.fat", &fn);
    EXPECT_EQ(fn, size);
    EXPECT_EQ(back[hello_off], 0xE5);
    free(back);

    fat_ctx_t* fresh = NULL;
    EXPECT_EQ(fat_open("/tmp/p6_sync.fat", &fresh), FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(fresh, FAT_CLUSTER_ROOT, "hello.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(count_dir_entries(fresh, FAT_CLUSTER_ROOT), 4);
    assert_read_matches_host(fresh, "test_5kb.txt", "test_5kb.txt", 4962);
    EXPECT_EQ(fat_lookup(fresh, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 8);
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

    EXPECT_EQ(fat_unlink(NULL, FAT_CLUSTER_ROOT, "hello.txt"),
              FAT_ERR_INVALID_ARG);

    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "noexist.txt"),
              FAT_ERR_NOT_FOUND);
    /* directories go through fat_rmdir */
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "dir1"), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_unlink(ctx, 8, "subdir1"), FAT_ERR_INVALID_ARG);
    /* the volume label never matches a lookup */
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "demof12"), FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, NULL), FAT_ERR_INVALID_ARG);

    /* the failures changed nothing */
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5);
    EXPECT_EQ(count_dir_entries(ctx, 8), 5);
    assert_read_string(ctx, "hello.txt", "hello world\n");
    fat_close(ctx);

    /* ATTR_READ_ONLY: the DOS access-denied equivalent */
    img = copy_image(orig, size);
    size_t hello_off = find_slot_in_region(img, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    EXPECT_TRUE(hello_off != 0);
    img[hello_off + 11] |= 0x01;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt"),
              FAT_ERR_INVALID_ARG);
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "empty.txt", NULL, 0,
                             NULL),
              FAT_OK);
    uint32_t free_before = count_free_clusters(ctx);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "empty.txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 0);

    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "empty.txt"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "empty.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(count_free_clusters(ctx), free_before); /* nothing to free */
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5); /* back to 5 */

    /* raw: the slot the empty file took (first 0x00 slot = #5, right
     * after DIR2) is a deleted slot now */
    EXPECT_EQ(fat_write(ctx, "/tmp/p6_undel12.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_undel12.fat", &fn);
    EXPECT_EQ(back[F12_ROOT_BYTES + 5u * 32u], 0xE5);
    free(back);
    remove("/tmp/p6_undel12.fat");

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    size_t hello_off = find_slot_in_region(orig, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    EXPECT_TRUE(hello_off != 0);
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt"), FAT_OK);

    fat_dirent_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.attributes = 0x20;
    tmpl.first_cluster = 2;
    tmpl.file_size = 8;
    EXPECT_EQ(fat_add_dirent(ctx, FAT_CLUSTER_ROOT, "fresh.txt", &tmpl),
              FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "fresh.txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(de.file_size, 8);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de),
              FAT_ERR_NOT_FOUND);

    /* raw: FRESH sits exactly where HELLO was */
    EXPECT_EQ(fat_write(ctx, "/tmp/p6_reuse6.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_reuse6.fat", &fn);
    size_t fresh_off = find_slot_in_region(back, F12_ROOT_BYTES, 112,
                                           "FRESH   TXT");
    EXPECT_EQ(fresh_off, hello_off);
    free(back);
    remove("/tmp/p6_reuse6.fat");

    /* the slot count is back to 5 */
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir1", &de), FAT_OK);
    uint32_t sub1 = de.first_cluster;
    EXPECT_EQ(fat_at(ctx, sub1), 0xFFF); /* single-cluster directory */

    uint32_t free_before = count_free_clusters(ctx);
    EXPECT_EQ(fat_rmdir(ctx, 8, "subdir1"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir1", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, sub1), 0); /* the directory cluster is free */
    EXPECT_EQ(count_free_clusters(ctx), free_before + 1u);
    EXPECT_EQ(count_dir_entries(ctx, 8), 4); /* . .. subdir2 hoge */

    /* raw: SUBDIR1's slot in dir1 is 0xE5, every other slot identical */
    EXPECT_EQ(fat_write(ctx, "/tmp/p6_rmdir12.fat"), FAT_OK);
    size_t fn = 0;
    uint8_t* back = read_image("/tmp/p6_rmdir12.fat", &fn);
    size_t dir1_off = 26u * 512u; /* cluster 8 = data sector 26 */
    size_t sub1_off = find_slot_in_region(orig, dir1_off, 32, "SUBDIR1    ");
    size_t sub2_off = find_slot_in_region(orig, dir1_off, 32, "SUBDIR2    ");
    size_t hoge_off = find_slot_in_region(orig, dir1_off, 32, "HOGE    TXT");
    EXPECT_TRUE(sub1_off && sub2_off && hoge_off);
    EXPECT_EQ(back[sub1_off], 0xE5);
    EXPECT_MEMEQ(back + sub2_off, orig + sub2_off, 32);
    EXPECT_MEMEQ(back + hoge_off, orig + hoge_off, 32);
    EXPECT_MEMEQ(back + dir1_off, orig + dir1_off, 64); /* "." ".." */
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;

    /* dir1 holds subdir1, subdir2, hoge.txt */
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "dir1"), FAT_ERR_DIR_NOT_EMPTY);
    /* dir2/subdir1 holds page.txt and test_5kb.txt */
    EXPECT_EQ(fat_rmdir(ctx, 11, "subdir1"), FAT_ERR_DIR_NOT_EMPTY);
    /* regular files are not directories */
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "hello.txt"),
              FAT_ERR_INVALID_ARG);
    /* the root cannot be removed, however it is named */
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "."), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_rmdir(ctx, 8, ".."), FAT_ERR_INVALID_ARG);
    /* lookup failures propagate */
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "noexist"), FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "demof12"), FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_rmdir(NULL, FAT_CLUSTER_ROOT, "dir1"), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, NULL), FAT_ERR_INVALID_ARG);

    /* nothing above changed the image */
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5);
    EXPECT_EQ(count_dir_entries(ctx, 8), 5);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 8);
    EXPECT_EQ(fat_at(ctx, 8), 0xFFF);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir2", &de), FAT_OK);
    uint32_t sub2 = de.first_cluster;
    EXPECT_EQ(fat_at(ctx, sub2), 0xFFF);

    EXPECT_EQ(fat_rmdir(ctx, 8, "subdir2"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/subdir2", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, sub2), 0);

    /* composed variant: dir2/subdir1 holds two files; unlinking both
     * leaves only "."/".." plus 0xE5 slots, and rmdir accepts that */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &de), FAT_OK);
    uint32_t d2s1 = de.first_cluster;
    EXPECT_EQ(fat_unlink(ctx, d2s1, "page.txt"), FAT_OK);
    EXPECT_EQ(fat_unlink(ctx, d2s1, "test_5kb.txt"), FAT_OK);
    EXPECT_EQ(count_dir_entries(ctx, d2s1), 2); /* just the dots */
    EXPECT_EQ(fat_rmdir(ctx, 11, "subdir1"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, d2s1), 0);

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

    EXPECT_EQ(fat_file_open_write(NULL, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_ERR_INVALID_ARG);

    uint8_t* img = copy_image(orig, size);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "noexist.txt", &f),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "dir1", &f),
              FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_file_open_write(ctx, 8, "subdir1", &f), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "demof12", &f),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, NULL, &f),
              FAT_ERR_INVALID_ARG);

    /* success: cursor facts and read behaviour of fat_file_open */
    f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_OK);
    EXPECT_TRUE(f != NULL);
    EXPECT_EQ(fat_file_tell(f), 0);
    EXPECT_EQ(fat_file_size(f), 12);
    uint8_t buf[16];
    size_t got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, buf, sizeof(buf), &got), FAT_OK);
    EXPECT_EQ(got, 12);
    EXPECT_MEMEQ(buf, "hello world\n", 12);
    EXPECT_EQ(fat_file_tell(f), 12);
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, buf, 12, &got), FAT_OK);
    EXPECT_EQ(got, 12);
    EXPECT_MEMEQ(buf, "hello world\n", 12);
    EXPECT_EQ(fat_file_seek(f, 13), FAT_ERR_INVALID_ARG); /* past EOF */
    fat_file_close(f);
    fat_close(ctx);

    /* ATTR_READ_ONLY refuses (the write side of the DOS semantics) */
    img = copy_image(orig, size);
    size_t hello_off = find_slot_in_region(img, F12_ROOT_BYTES, 112,
                                           "HELLO   TXT");
    EXPECT_TRUE(hello_off != 0);
    img[hello_off + 11] |= 0x01;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_ERR_INVALID_ARG);
    fat_close(ctx);

    /* a subdirectory binding reads the file in that directory */
    img = copy_image(orig, size);
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, 12, "page.txt", &f), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 14);
    got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, buf, 14, &got), FAT_OK);
    EXPECT_EQ(got, 14);
    EXPECT_MEMEQ(buf, "You are page.\n", 14);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, 12, "test_5kb.txt", slot_before);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/test_5kb.txt",
                         &de),
              FAT_OK);
    uint32_t chain[8];
    uint32_t nchain = record_chain(ctx, de.first_cluster, chain, 8);
    EXPECT_EQ(nchain, 5); /* 4962 bytes over 1024B clusters */
    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, 12, "test_5kb.txt", &f), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 4962);
    EXPECT_EQ(fat_file_seek(f, 4000), FAT_OK);

    EXPECT_EQ(fat_file_truncate(f, 1000), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 1000);
    EXPECT_EQ(fat_file_tell(f), 1000); /* clamped down from 4000 */

    /* the chain is one cluster now; the four tail clusters are FREE */
    EXPECT_EQ(fat_at(ctx, chain[0]), 0xFFF);
    for (uint32_t i = 1; i < nchain; i++)
        EXPECT_EQ(fat_at(ctx, chain[i]), 0);
    EXPECT_EQ(count_free_clusters(ctx), free_before + 4u);

    /* the kept prefix still reads back byte-exact through the cursor */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    uint8_t* want = malloc(1000);
    uint8_t* buf = malloc(1000);
    EXPECT_TRUE(want != NULL && buf != NULL);
    read_host_prefix(1000, want);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, 1000, &got), FAT_OK);
    EXPECT_EQ(got, 1000);
    EXPECT_MEMEQ(buf, want, 1000);
    /* EOF right after */
    got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, buf, 1, &got), FAT_OK);
    EXPECT_EQ(got, 0);
    free(want);
    free(buf);

    /* a position below the new size keeps its offset */
    EXPECT_EQ(fat_file_seek(f, 10), FAT_OK);
    EXPECT_EQ(fat_file_truncate(f, 500), FAT_OK);
    EXPECT_EQ(fat_file_tell(f), 10);
    EXPECT_EQ(fat_file_size(f), 500);
    fat_file_close(f);

    /* the slot changed in exactly the two sanctioned fields */
    uint8_t slot_after[32];
    read_slot_raw(ctx, 12, "test_5kb.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, 500);

    /* and a fresh lookup agrees */
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir2/subdir1/test_5kb.txt",
                         &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 500);
    EXPECT_EQ(de.first_cluster, chain[0]);

    /* the root copy is untouched */
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 4962);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_before);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    uint32_t chain[16];
    uint32_t nchain = record_chain(ctx, de.first_cluster, chain, 16);
    EXPECT_EQ(nchain, 5);
    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_truncate(f, 6000), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 6000);
    EXPECT_EQ(fat_file_tell(f), 0);

    /* 6000 bytes = 6 clusters of 1024: one new cluster, chain linked */
    EXPECT_EQ(chain_length(ctx, de.first_cluster), 6);
    EXPECT_TRUE(fat_at(ctx, chain[nchain - 1]) != 0xFFF); /* old tail on */
    EXPECT_EQ(count_free_clusters(ctx), free_before - 1u);

    /* whole content: the original 4962 bytes, then 1038 zeros */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    uint8_t* buf = malloc(6000);
    uint8_t* want = malloc(6000);
    EXPECT_TRUE(buf != NULL && want != NULL);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, 6000, &got), FAT_OK);
    EXPECT_EQ(got, 6000);
    read_host_prefix(4962, want);
    memset(want + 4962, 0, 1038);
    EXPECT_MEMEQ(buf, want, 6000);
    free(want);
    free(buf);

    /* zeros strictly past the old end, through a seek too */
    EXPECT_EQ(fat_file_seek(f, 4962), FAT_OK);
    uint8_t z = 0xAA;
    got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, &z, 1, &got), FAT_OK);
    EXPECT_EQ(got, 1);
    EXPECT_EQ(z, 0);
    fat_file_close(f);

    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, 6000);

    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 6000);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_before);

    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_seek(f, 5), FAT_OK);
    EXPECT_EQ(fat_file_truncate(f, 0), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 0);
    EXPECT_EQ(fat_file_tell(f), 0); /* clamped to 0 */
    EXPECT_EQ(fat_at(ctx, 2), 0);   /* hello's only cluster freed */
    EXPECT_EQ(count_free_clusters(ctx), free_before + 1u);

    /* empty reads EOF */
    uint8_t b = 0x41;
    size_t got = 0x5A5A;
    EXPECT_EQ(fat_file_read(f, &b, 1, &got), FAT_OK);
    EXPECT_EQ(got, 0);

    /* the slot: first_cluster 0 ON DISK, everything else untouched */
    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after, 0, 0);

    /* growing the empty file allocates a fresh chain */
    EXPECT_EQ(fat_file_truncate(f, 2000), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 2000);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 2000);
    EXPECT_TRUE(de.first_cluster >= 2u); /* a real chain was allocated */
    EXPECT_TRUE(de.first_cluster != 2u || fat_at(ctx, 2) != 0);
    EXPECT_EQ(chain_length(ctx, de.first_cluster), 2);
    EXPECT_EQ(count_free_clusters(ctx), free_before - 1u);

    /* and it reads back as zeros */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    uint8_t* buf = malloc(2000);
    EXPECT_TRUE(buf != NULL);
    memset(buf, 0x5A, 2000);
    got = 0;
    EXPECT_EQ(fat_file_read(f, buf, 2000, &got), FAT_OK);
    EXPECT_EQ(got, 2000);
    for (int i = 0; i < 2000; i++)
        EXPECT_EQ(buf[i], 0);
    free(buf);
    fat_file_close(f);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", slot_before);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    EXPECT_EQ(chain_length(ctx, de.first_cluster), 5);
    uint32_t free_before = count_free_clusters(ctx);

    size_t n = 4962;
    uint8_t* data = make_pattern(n);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f),
              FAT_OK);

    /* len == 0: OK, nothing written, nothing moved */
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, data, 0, &w), FAT_OK);
    EXPECT_EQ(w, 0);
    EXPECT_EQ(fat_file_tell(f), 0);
    EXPECT_EQ(fat_file_size(f), n);

    EXPECT_EQ(fat_file_write(f, data, n, &w), FAT_OK);
    EXPECT_EQ(w, n); /* no short writes */
    EXPECT_EQ(fat_file_tell(f), n);
    EXPECT_EQ(fat_file_size(f), n); /* size unchanged */

    /* same five clusters, no allocation, no free */
    EXPECT_EQ(chain_length(ctx, de.first_cluster), 5);
    EXPECT_EQ(count_free_clusters(ctx), free_before);

    /* read-back through the same cursor */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    uint8_t* buf = malloc(n);
    EXPECT_TRUE(buf != NULL);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, n, &got), FAT_OK);
    EXPECT_EQ(got, n);
    EXPECT_MEMEQ(buf, data, n);
    free(buf);
    fat_file_close(f);

    /* and through a fresh whole-file read */
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    uint8_t* back = NULL;
    size_t bn = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &back, &bn), FAT_OK);
    EXPECT_EQ(bn, n);
    EXPECT_MEMEQ(back, data, n);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint8_t slot_before[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_before);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    uint32_t free_before = count_free_clusters(ctx);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_seek(f, 12), FAT_OK); /* EOF is a legal position */

    size_t n = 3000;
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, data, n, &w), FAT_OK);
    EXPECT_EQ(w, n);
    EXPECT_EQ(fat_file_tell(f), 3012);
    EXPECT_EQ(fat_file_size(f), 3012);
    EXPECT_EQ(chain_length(ctx, de.first_cluster), 3); /* 3012B/1024B */
    EXPECT_EQ(count_free_clusters(ctx), free_before - 2u);

    /* content: the greeting, then the pattern */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    uint8_t* buf = malloc(3012);
    EXPECT_TRUE(buf != NULL);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, 3012, &got), FAT_OK);
    EXPECT_EQ(got, 3012);
    EXPECT_MEMEQ(buf, "hello world\n", 12);
    EXPECT_MEMEQ(buf + 12, data, n);
    free(buf);
    fat_file_close(f);

    uint8_t slot_after[32];
    read_slot_raw(ctx, FAT_CLUSTER_ROOT, "hello.txt", slot_after);
    assert_slot_only_size_cluster_changed(slot_before, slot_after,
                                          de.first_cluster, 3012);

    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 3012);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    uint32_t free_before = count_free_clusters(ctx);

    uint8_t* patch = make_pattern(100);
    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_seek(f, 1000), FAT_OK);
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, patch, 100, &w), FAT_OK);
    EXPECT_EQ(w, 100);
    EXPECT_EQ(fat_file_tell(f), 1100);
    EXPECT_EQ(fat_file_size(f), 4962); /* size unchanged */
    EXPECT_EQ(chain_length(ctx, de.first_cluster), 5);
    EXPECT_EQ(count_free_clusters(ctx), free_before);

    /* expected: host[0..1000) + patch + host[1100..4962) */
    uint8_t* want = malloc(4962);
    uint8_t* buf = malloc(4962);
    EXPECT_TRUE(want != NULL && buf != NULL);
    read_host_prefix(4962, want);
    memcpy(want + 1000, patch, 100);
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, 4962, &got), FAT_OK);
    EXPECT_EQ(got, 4962);
    EXPECT_MEMEQ(buf, want, 4962);
    free(want);
    free(buf);
    fat_file_close(f);

    /* the whole-file read agrees */
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    uint8_t* back = NULL;
    size_t bn = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &back, &bn), FAT_OK);
    EXPECT_EQ(bn, 4962);
    EXPECT_MEMEQ(back + 1000, patch, 100);
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
    EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_seek(f, 4962), FAT_OK); /* write at EOF */

    size_t n = 3300; /* 4962+3300 = 8262 B = 9 clusters: 4 new ones */
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    fat_result_t r = fat_file_write(f, data, n, &w);
    EXPECT_EQ(r, FAT_ERR_IO);
    EXPECT_TRUE(w <= n);
    EXPECT_TRUE(io.write_calls > 0); /* the backend was actually asked */
    EXPECT_EQ(io.oob, 0);

    /* consistency: the reported size covers exactly the prefix (no lookups
     * afterwards -- reading a fresh sector could evict another dirty slot
     * and fail on its own, which a correct implementation may freely do) */
    EXPECT_EQ(fat_file_size(f), 4962u + w);

    /* the failed flushes stay dirty: sync retries and fails again */
    EXPECT_EQ(fat_sync(ctx), FAT_ERR_IO);

    fat_file_close(f);
    fat_close(ctx);
    EXPECT_EQ(io.close_calls, 1);
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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_seek(f, 4962), FAT_OK);

    size_t n = 3300; /* needs 4 new clusters, only 2 are free */
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, data, n, &w), FAT_ERR_DISK_FULL);
    EXPECT_TRUE(w <= 2206u);
    EXPECT_EQ(fat_file_size(f), 4962u + w);

    /* the prefix is consistent and readable end to end */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    size_t total = 4962u + w;
    uint8_t* buf = malloc(total);
    uint8_t* want = malloc(total);
    EXPECT_TRUE(buf != NULL && want != NULL);
    read_host_prefix(4962, want);
    if (w > 0)
        memcpy(want + 4962, data, w);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, total, &got), FAT_OK);
    EXPECT_EQ(got, total);
    EXPECT_MEMEQ(buf, want, total);
    free(buf);
    free(want);
    fat_file_close(f);

    /* a fresh lookup agrees on the truncated-but-consistent size */
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 4962u + w);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    uint8_t b = 'x';
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(NULL, &b, 1, &w), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_file_truncate(NULL, 0), FAT_ERR_INVALID_ARG);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_write(f, NULL, 1, &w), FAT_ERR_INVALID_ARG);
    EXPECT_EQ(fat_file_write(f, &b, 1, NULL), FAT_ERR_INVALID_ARG);
    /* the rejected calls moved nothing */
    EXPECT_EQ(fat_file_tell(f), 0);
    EXPECT_EQ(fat_file_size(f), 12);
    w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, &b, 0, &w), FAT_OK);
    EXPECT_EQ(w, 0);
    EXPECT_EQ(fat_file_size(f), 12);
    fat_file_close(f);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    const fat_geometry_t* g = fat_geometry(ctx);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    /* hello.txt: one cluster */
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "hello.txt"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, 3), 0);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u + 1u);
    EXPECT_TRUE(fi.next_free_cluster >= 2u);
    EXPECT_TRUE(fi.next_free_cluster <= g->cluster_count + 1u);

    /* test_5kb.txt: the 10-cluster chain 4..13 frees entirely */
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "test_5kb.txt"), FAT_OK);
    for (uint32_t c = 4u; c <= 13u; c++)
        EXPECT_EQ(fat_at(ctx, c), 0);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u + 11u);
    /* the FSInfo now tells the truth of the FAT itself */
    EXPECT_EQ(count_free_clusters(ctx), 66454u + 11u);

    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 42);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 57);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);

    /* the fixture's sub1 holds page.txt (cluster 58) -- phase 2 pins that.
     * unlink it first, then the dots-only directory is removable */
    EXPECT_EQ(fat_unlink(ctx, 57, "page.txt"), FAT_OK);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u + 1u);

    EXPECT_EQ(fat_rmdir(ctx, 56, "sub1"), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "dir1/sub1", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, 57), 0);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u + 2u);

    /* non-empty still refuses: dir1 itself */
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "dir1"), FAT_ERR_DIR_NOT_EMPTY);

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_size(f), 12); /* one 512B cluster */

    /* grow to 1500 = 3 clusters: two allocations */
    EXPECT_EQ(fat_file_truncate(f, 1500), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 1500);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u - 2u);
    EXPECT_EQ(fat_file_seek(f, 12), FAT_OK);
    uint8_t z[1488];
    memset(z, 0x5A, sizeof(z));
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, z, sizeof(z), &got), FAT_OK);
    EXPECT_EQ(got, sizeof(z));
    for (size_t i = 0; i < sizeof(z); i++)
        EXPECT_EQ(z[i], 0);

    /* shrink to 100 = 1 cluster: both extensions free again */
    EXPECT_EQ(fat_file_seek(f, 1400), FAT_OK);
    EXPECT_EQ(fat_file_truncate(f, 100), FAT_OK);
    EXPECT_EQ(fat_file_tell(f), 100); /* clamped */
    EXPECT_EQ(fat_file_size(f), 100);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);
    fat_file_close(f);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 100);
    EXPECT_EQ(de.first_cluster, 3); /* the original cluster survived */

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
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_fsinfo_t fi;
    memset(&fi, 0, sizeof(fi));
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "hello.txt", &f),
              FAT_OK);
    EXPECT_EQ(fat_file_size(f), 12);
    EXPECT_EQ(fat_file_seek(f, 12), FAT_OK);

    size_t n = 2000;
    uint8_t* data = make_pattern(n);
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, data, n, &w), FAT_OK);
    EXPECT_EQ(w, n);
    EXPECT_EQ(fat_file_size(f), 2012); /* 2012 B = 4 clusters of 512 */
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u - 3u);

    /* read-back: the greeting, then the pattern */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    uint8_t* back = malloc(2012);
    EXPECT_TRUE(back != NULL);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, back, 2012, &got), FAT_OK);
    EXPECT_EQ(got, 2012);
    EXPECT_MEMEQ(back, "hello world\n", 12);
    EXPECT_MEMEQ(back + 12, data, n);
    free(back);

    /* truncate back to one cluster and watch the FSInfo reverse */
    EXPECT_EQ(fat_file_truncate(f, 512), FAT_OK);
    EXPECT_EQ(fat_file_size(f), 512);
    EXPECT_EQ(fat_fsinfo(ctx, &fi), FAT_OK);
    EXPECT_EQ(fi.free_cluster_count, 66454u);
    fat_file_close(f);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 512);
    EXPECT_EQ(de.first_cluster, 3);

    free(data);
    fat_close(ctx);
    free(orig);
}

void test_write_register(void)
{
    REGISTER(test_set_fat_entry12);
    REGISTER(test_alloc_cluster12);
    REGISTER(test_free_chain12);
    REGISTER(test_add_dirent_create);
    REGISTER(test_add_dirent_reuse);
    REGISTER(test_add_dirent_errors);
    REGISTER(test_write_file_roundtrip12);
    REGISTER(test_write_file_boundaries);
    REGISTER(test_write_file_small);
    REGISTER(test_write_file_rollback);
    REGISTER(test_write_file_subdir);
    REGISTER(test_write_flush);
    REGISTER_AS(test_fat16_write, NEED_FAT16);
    REGISTER_AS(test_set_fat_entry32, NEED_FAT32);
    REGISTER_AS(test_alloc_free32, NEED_FAT32);
    REGISTER_AS(test_fat32_root_extend, NEED_FAT32);
    REGISTER_AS(test_fat32_write_file, NEED_FAT32);
    REGISTER(test_unlink_basic12);
    REGISTER(test_unlink_sync_reopen);
    REGISTER(test_unlink_errors12);
    REGISTER(test_unlink_empty_file12);
    REGISTER(test_unlink_slot_reuse);
    REGISTER(test_rmdir_empty12);
    REGISTER(test_rmdir_errors12);
    REGISTER(test_rmdir_deleted_slots12);
    REGISTER(test_open_write_basic12);
    REGISTER(test_truncate_shrink12);
    REGISTER(test_truncate_grow12);
    REGISTER(test_truncate_zero12);
    REGISTER(test_write_inplace12);
    REGISTER(test_write_extend12);
    REGISTER(test_write_middle12);
    REGISTER(test_write_fail_io12);
    REGISTER(test_write_disk_full_prefix12);
    REGISTER(test_write_null_args12);
    REGISTER_AS(test_unlink_fat32, NEED_FAT32);
    REGISTER_AS(test_rmdir_fat32, NEED_FAT32);
    REGISTER_AS(test_truncate_fat32, NEED_FAT32);
    REGISTER_AS(test_write_fat32, NEED_FAT32);
}
