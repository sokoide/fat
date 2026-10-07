// tests/test_io.c -- the I/O abstraction group: fat_io_t backends,
// fat_open_io, fat_sync, write-back visibility, and failure injection.

#include "test_util.h"

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
    EXPECT_TRUE(vtbl->read != NULL && vtbl->write != NULL);
    EXPECT_EQ(vtbl->size(vtbl), size);

    uint8_t tmp[8];
    memset(tmp, 0xAA, sizeof(tmp));
    EXPECT_EQ(vtbl->read(vtbl, 0, tmp, 0), FAT_OK);
    EXPECT_EQ(vtbl->write(vtbl, 0, tmp, 0), FAT_OK);
    EXPECT_EQ(vtbl->read(vtbl, 100, tmp, sizeof(tmp)), FAT_OK);
    EXPECT_MEMEQ(tmp, buf + 100, sizeof(tmp));

    EXPECT_EQ(vtbl->read(vtbl, size - 1, tmp, 2), FAT_ERR_IO);
    EXPECT_EQ(vtbl->write(vtbl, size, "x", 1), FAT_ERR_IO);
    EXPECT_EQ(vtbl->read(vtbl, size + 512, tmp, 1), FAT_ERR_IO);
    EXPECT_EQ(io.read_calls, 4);
    EXPECT_EQ(io.write_calls, 2);
    EXPECT_EQ(io.oob, 3);
    /* the failed transfers did not touch the buffer */
    EXPECT_MEMEQ(tmp, buf + 100, sizeof(tmp));

    vtbl->close(vtbl);
    EXPECT_EQ(io.close_calls, 1);

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

    EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);
    EXPECT_TRUE(ctx != NULL);
    /* "the image is not loaded whole" (fat.h): open reads the boot
     * sector, not the whole 720KB image */
    EXPECT_TRUE(log_bytes(io.reads, io.nreads) < size);
    EXPECT_EQ(io.write_calls, 0);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, 12);
    /* the dirent bytes can only come from the fixed root region */
    EXPECT_TRUE(log_touches(io.reads, io.nreads, F12_ROOT_BYTES,
                            F12_DATA_BYTES));
    EXPECT_EQ(io.write_calls, 0); /* reads alone never flush anything */
    EXPECT_EQ(io.oob, 0);

    uint32_t v = 0;
    EXPECT_EQ(fat_get_fat_entry(ctx, 3, &v), FAT_OK);
    EXPECT_EQ(v, 4);
    EXPECT_TRUE(log_touches(io.reads, io.nreads, FIXTURE_FAT_SECTOR * 512u,
                            F12_ROOT_BYTES));

    fat_close(ctx);
    EXPECT_EQ(io.close_calls, 1); /* exactly once, at fat_close */
    EXPECT_EQ(io.oob, 0);

    /* close() == NULL is legal (nothing to release): fat_close must
     * not call it */
    CountIo nc;
    countio_init(&nc, buf, size);
    nc.io.close = NULL;
    fat_ctx_t* nctx = NULL;
    EXPECT_EQ(fat_open_io(&nc.io, &nctx), FAT_OK);
    EXPECT_EQ(fat_lookup(nctx, FAT_CLUSTER_ROOT, "hello.txt", &de), FAT_OK);
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
    EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_ERR_INVALID_BPB);
    EXPECT_EQ(io.close_calls, 0); /* the context never took ownership */
    io.io.close(&io.io);          /* the caller releases it themselves */
    EXPECT_EQ(io.close_calls, 1);
    free(buf);

    CountIo tiny;
    countio_init(&tiny, orig, 100);
    fat_ctx_t* tctx = NULL;
    EXPECT_EQ(fat_open_io(&tiny.io, &tctx), FAT_ERR_IO);
    EXPECT_EQ(tiny.close_calls, 0);
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
    EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);

    /* the write lives in the cache: byte-identical backend, no io->write
     * call, but already visible through the context */
    EXPECT_EQ(fat_set_fat_entry(ctx, 53, 0x235), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 53), 0x235);
    EXPECT_EQ(io.write_calls, 0);
    EXPECT_MEMEQ(buf, snap, size);

    /* sync flushes: both FAT tables carry the new value in their bytes */
    EXPECT_EQ(fat_sync(ctx), FAT_OK);
    EXPECT_TRUE(io.write_calls > 0);
    EXPECT_EQ(raw_fat12_at(buf, FIXTURE_FAT_SECTOR, 53), 0x235);
    EXPECT_EQ(raw_fat12_at(buf, 4u, 53), 0x235);
    EXPECT_EQ(io.close_calls, 0); /* sync does not release anything */

    /* a second write is held again, and fat_close flushes it */
    memcpy(snap, buf, size);
    EXPECT_EQ(fat_set_fat_entry(ctx, 53, 0x146), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 53), 0x146);
    EXPECT_MEMEQ(buf, snap, size);
    fat_close(ctx);
    EXPECT_EQ(io.close_calls, 1);
    EXPECT_EQ(raw_fat12_at(buf, FIXTURE_FAT_SECTOR, 53), 0x146);
    EXPECT_EQ(raw_fat12_at(buf, 4u, 53), 0x146);

    /* the flushed backend equals the whole-image export of a context
     * that performed the same operation (fat_write ground truth) */
    fat_ctx_t* ref = NULL;
    EXPECT_EQ(fat_open_mem(orig, size, &ref), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ref, 53, 0x146), FAT_OK);
    EXPECT_EQ(fat_write(ref, "/tmp/p5_wb_ref.fat"), FAT_OK);
    size_t rn = 0;
    uint8_t* want = read_image("/tmp/p5_wb_ref.fat", &rn);
    EXPECT_EQ(rn, size);
    EXPECT_MEMEQ(buf, want, size);
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
    EXPECT_EQ(fat_io_mem(&io, orig, size), FAT_OK);
    EXPECT_TRUE(io != NULL);
    EXPECT_EQ(io->size(io), size);

    uint8_t tmp[16];
    EXPECT_EQ(io->read(io, 0, tmp, sizeof(tmp)), FAT_OK);
    EXPECT_MEMEQ(tmp, orig, sizeof(tmp));

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_io(io, &ctx), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, 53, 0x235), FAT_OK);
    EXPECT_EQ(fat_sync(ctx), FAT_OK);

    /* entry 53 is odd: FAT bytes 79..80 decode as (b0>>4)|(b1<<4) */
    uint8_t e[2];
    EXPECT_EQ(io->read(io, FIXTURE_FAT_SECTOR * 512u + 53u * 3u / 2u, e,
                       sizeof(e)),
              FAT_OK);
    EXPECT_EQ((uint16_t)((e[0] >> 4) | ((uint16_t)e[1] << 4)), 0x235);

    /* the caller's original buffer survived it all */
    EXPECT_MEMEQ(orig, snap, size);

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
    EXPECT_EQ(fat_io_file(&nope, "/tmp/p5_no_such_backend.fat"), FAT_ERR_IO);

    write_image("/tmp/p5_fio_rt.fat", orig, size);
    fat_io_t* io = NULL;
    EXPECT_EQ(fat_io_file(&io, "/tmp/p5_fio_rt.fat"), FAT_OK);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_io(io, &ctx), FAT_OK);

    size_t n = 5000;
    uint8_t* data = make_pattern(n);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);
    EXPECT_EQ(fat_sync(ctx), FAT_OK);
    fat_close(ctx); /* close flushes too; nothing may be dirty now */

    /* expected on-disk state: the same write, exported */
    fat_ctx_t* ref = NULL;
    EXPECT_EQ(fat_open_mem(orig, size, &ref), FAT_OK);
    EXPECT_EQ(fat_write_file(ref, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);
    EXPECT_EQ(fat_write(ref, "/tmp/p5_fio_ref.fat"), FAT_OK);
    size_t gn = 0;
    size_t rn = 0;
    uint8_t* got = read_image("/tmp/p5_fio_rt.fat", &gn);
    uint8_t* want = read_image("/tmp/p5_fio_ref.fat", &rn);
    EXPECT_TRUE(gn == size && rn == size);
    EXPECT_MEMEQ(got, want, size);

    /* a fresh fat_open (file backend internally) reads the result */
    fat_ctx_t* fresh = NULL;
    EXPECT_EQ(fat_open("/tmp/p5_fio_rt.fat", &fresh), FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(fresh, FAT_CLUSTER_ROOT, "new.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, n);
    uint8_t* back = NULL;
    size_t bn = 0;
    EXPECT_EQ(fat_read_file(fresh, &de, &back, &bn), FAT_OK);
    EXPECT_EQ(bn, n);
    EXPECT_MEMEQ(back, data, n);
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
    EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);

    EXPECT_EQ(fat_set_fat_entry(ctx, 53, 0x235), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 53), 0x235); /* cached; the flush comes later */
    EXPECT_EQ(fat_sync(ctx), FAT_ERR_IO);
    EXPECT_TRUE(io.write_calls > 0); /* the backend was actually asked */

    fat_close(ctx); /* void: best-effort flush, no double close */
    EXPECT_EQ(io.close_calls, 1);

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
        EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);
        uint32_t v = 0;
        EXPECT_EQ(fat_get_fat_entry(ctx, 2, &v), FAT_ERR_IO);
        fat_close(ctx);
        EXPECT_EQ(io.close_calls, 1);
        free(buf);
    }
    {
        /* root region unreadable: iterating the fixed root must fail */
        uint8_t* buf = copy_image(orig, size);
        CountIo io;
        countio_init(&io, buf, size);
        io.fail_reads_at = F12_ROOT_BYTES;
        fat_ctx_t* ctx = NULL;
        EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);
        int n = 0;
        EXPECT_EQ(iter_dir(ctx, FAT_CLUSTER_ROOT, count_any_cb, &n),
                  FAT_ERR_IO);
        fat_close(ctx);
        EXPECT_EQ(io.close_calls, 1);
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
        EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);
        fat_dirent_t de;
        memset(&de, 0, sizeof(de));
        EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "hello.txt", &de),
                  FAT_OK);
        EXPECT_EQ(de.file_size, 12);
        uint8_t* data = NULL;
        size_t nn = 0;
        EXPECT_EQ(fat_read_file(ctx, &de, &data, &nn), FAT_ERR_IO);
        EXPECT_TRUE(data == NULL);
        fat_close(ctx);
        EXPECT_EQ(io.close_calls, 1);
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
    EXPECT_EQ(fat_open_io(&io.io, &ctx), FAT_OK);
    const fat_geometry_t* g = fat_geometry(ctx);

    /* preconditions measured on the fixture: the FAT table covers byte
     * 513 (3 sectors = 1536 bytes) and 342 is a valid data cluster */
    EXPECT_TRUE(F12_ROOT_BYTES - FIXTURE_FAT_SECTOR * 512u >= 513u);
    EXPECT_TRUE(g->cluster_count + 1u >= 342u);
    EXPECT_EQ(fat_at(ctx, 340), 0);
    EXPECT_EQ(fat_at(ctx, 341), 0);
    EXPECT_EQ(fat_at(ctx, 342), 0);

    EXPECT_EQ(fat_set_fat_entry(ctx, 340, 0x123), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, 342, 0x456), FAT_OK);
    EXPECT_EQ(fat_set_fat_entry(ctx, 341, 0xABC), FAT_OK);
    EXPECT_EQ(fat_at(ctx, 340), 0x123);
    EXPECT_EQ(fat_at(ctx, 341), 0xABC);
    EXPECT_EQ(fat_at(ctx, 342), 0x456);

    /* flush and byte-verify both FAT tables (base sectors 1 and 4): the
     * neighbours decode exactly, which is only possible when the
     * two-sector RMW preserved every shared nibble */
    EXPECT_EQ(fat_sync(ctx), FAT_OK);
    for (unsigned t = 0; t < 2u; t++) {
        unsigned base = t == 0u ? FIXTURE_FAT_SECTOR : 4u;
        EXPECT_EQ(raw_fat12_at(buf, base, 340), 0x123);
        EXPECT_EQ(raw_fat12_at(buf, base, 341), 0xABC);
        EXPECT_EQ(raw_fat12_at(buf, base, 342), 0x456);
    }
    /* the straddled pair itself: image byte 1023 carries 0x123's high
     * nibble below 0xABC's low nibble, byte 1024 the rest of 0xABC */
    EXPECT_EQ(buf[1023],
              (uint8_t)(((0x123u >> 8) & 0x0Fu) | ((0xABCu & 0x0Fu) << 4)));
    EXPECT_EQ(buf[1024], (uint8_t)(0xABCu >> 4));

    fat_close(ctx);
    EXPECT_EQ(io.close_calls, 1);

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
    EXPECT_EQ(fat_open("/tmp/p5_reach.fat", &ctx), FAT_OK);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);
    fat_close(ctx); /* the flush point */

    /* expected on-disk bytes: the same operation, exported */
    fat_ctx_t* ref = NULL;
    EXPECT_EQ(fat_open_mem(orig, size, &ref), FAT_OK);
    EXPECT_EQ(fat_write_file(ref, FAT_CLUSTER_ROOT, "new.txt", data, n,
                             NULL),
              FAT_OK);
    EXPECT_EQ(fat_write(ref, "/tmp/p5_reach_ref.fat"), FAT_OK);
    size_t gn = 0;
    size_t rn = 0;
    uint8_t* got = read_image("/tmp/p5_reach.fat", &gn);
    uint8_t* want = read_image("/tmp/p5_reach_ref.fat", &rn);
    EXPECT_TRUE(gn == size && rn == size);
    EXPECT_MEMEQ(got, want, size);

    /* and a fresh reader sees the written file */
    fat_ctx_t* fresh = NULL;
    EXPECT_EQ(fat_open("/tmp/p5_reach.fat", &fresh), FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(fresh, FAT_CLUSTER_ROOT, "new.txt", &de), FAT_OK);
    EXPECT_EQ(de.file_size, n);
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

void test_io_register(void)
{
    REGISTER(test_io_backend_semantics);
    REGISTER(test_open_io_calls);
    REGISTER(test_open_io_fail_paths);
    REGISTER(test_sync_writeback);
    REGISTER(test_io_mem_backend);
    REGISTER(test_io_file_roundtrip);
    REGISTER(test_write_fail_inject);
    REGISTER(test_read_fail_inject);
    REGISTER(test_fat12_straddle);
    REGISTER(test_open_write_reaches_file);
}
