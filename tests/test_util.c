// test_util.c -- framework implementation (registration, failure
// isolation, runner) and the helpers shared across the test groups.

#include "test_util.h"

#include <setjmp.h>
#include <stdarg.h>

/* --- framework ----------------------------------------------------------- */

typedef struct {
    const char* name;
    test_fn fn;
    unsigned needs;
} test_entry_t;

#define TEST_MAX 256

static test_entry_t tests[TEST_MAX];
static int test_count;

static jmp_buf test_jmp;
static int in_test;

void test_add(const char* name, test_fn fn, unsigned needs)
{
    if (test_count >= TEST_MAX) {
        fprintf(stderr, "test_add: test list full (%d)\n", TEST_MAX);
        exit(1);
    }
    tests[test_count].name = name;
    tests[test_count].fn = fn;
    tests[test_count].needs = needs;
    test_count++;
}

void test_fail(const char* file, int line, const char* fmt, ...)
{
    va_list ap;

    /* the runner already printed the test name without a newline */
    printf("FAILED\n    %s:%d: ", file, line);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');

    if (in_test)
        longjmp(test_jmp, 1); /* abandon only this test */
    exit(2); /* called outside the runner: nothing left to catch it */
}

/* 0 when a needs flag is unmet; *why receives the skip reason */
static int needs_met(unsigned needs, const char** why)
{
    if ((needs & NEED_MTOOLS) && !mtools_present()) {
        *why = "mtools not installed";
        return 0;
    }
    if ((needs & NEED_FAT16) && !fixture_present(IMG16_NAME)) {
        *why = "demof16.fat missing; run: make fat16";
        return 0;
    }
    if ((needs & NEED_FAT32) && !fixture_present(IMG32_NAME)) {
        *why = "demof32.fat missing; run: make fat32";
        return 0;
    }
    return 1;
}

int test_run_all(void)
{
    int failed = 0;
    int skipped = 0;

    for (int i = 0; i < test_count; i++) {
        const char* why = NULL;
        if (!needs_met(tests[i].needs, &why)) {
            printf("%-40s skipped (%s)\n", tests[i].name, why);
            skipped++;
            continue;
        }
        printf("%-40s", tests[i].name);
        fflush(stdout);
        if (setjmp(test_jmp) == 0) {
            in_test = 1;
            tests[i].fn();
            in_test = 0;
            puts(" ok");
        } else {
            /* the failure detail was already printed by test_fail */
            in_test = 0;
            failed++;
        }
    }
    printf("\n%d passed, %d skipped, %d failed\n", test_count - skipped - failed,
           skipped, failed);
    return failed;
}

/* ------------------------------------------------------------------ */
/* image I/O and fixtures                                              */
/* ------------------------------------------------------------------ */

fat_ctx_t* open_image(const char* name)
{
    fat_ctx_t* ctx = NULL;
    fat_result_t r = fat_open(name, &ctx);
    if (r != FAT_OK)
        fprintf(stderr, "%s: %s\n", name, fat_strerror(r));
    EXPECT_EQ(r, FAT_OK);
    EXPECT_TRUE(ctx != NULL);
    return ctx;
}

/* fat_iter_dir left the public API (cursor-only iteration, REVIEW.md §31);
 * this cursor-driven bridge keeps the suite's callback-style listings
 * until the test restructure retires them. */
fat_result_t iter_dir(fat_ctx_t* ctx, uint32_t dir_cluster, iter_cb cb,
                      void* user_data)
{
    if (ctx == NULL || cb == NULL)
        return FAT_ERR_INVALID_ARG;
    fat_dir_t* d = NULL;
    fat_result_t r = fat_dir_open(ctx, dir_cluster, &d);
    if (r != FAT_OK)
        return r;
    const fat_dirent_t* entry;
    const uint8_t* raw32;
    while ((r = fat_dir_next(d, &entry, &raw32)) == FAT_OK)
        cb(entry, raw32, user_data);
    fat_dir_close(d);
    return r == FAT_ERR_END_OF_DIR ? FAT_OK : r;
}

fat_ctx_t* open_fixture(void)
{
    return open_image(IMG_NAME);
}

/* 1 when the fixture image exists: missing FAT16/FAT32 suites are skipped
 * with a notice instead of failing (plain `make check` stays fast) */
int fixture_present(const char* name)
{
    FILE* fp = fopen(name, "rb");
    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

/* fat_get_fat_entry that insists on success and returns the value */
uint32_t fat_at(const fat_ctx_t* ctx, uint32_t cluster)
{
    uint32_t v = 0;
    EXPECT_EQ(fat_get_fat_entry(ctx, cluster, &v), FAT_OK);
    return v;
}

uint8_t* read_image(const char* name, size_t* out_size)
{
    FILE* fp = fopen(name, "rb");
    if (fp == NULL)
        perror(name);
    EXPECT_TRUE(fp != NULL);
    EXPECT_EQ(fseek(fp, 0, SEEK_END), 0);
    long n = ftell(fp);
    EXPECT_TRUE(n > 0);
    EXPECT_EQ(fseek(fp, 0, SEEK_SET), 0);
    uint8_t* buf = malloc((size_t)n);
    EXPECT_TRUE(buf != NULL);
    EXPECT_EQ(fread(buf, 1, (size_t)n, fp), (size_t)n);
    fclose(fp);
    *out_size = (size_t)n;
    return buf;
}

uint8_t* read_fixture(size_t* out_size)
{
    return read_image(IMG_NAME, out_size);
}

uint64_t checksum(const uint8_t* p, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += p[i];
    return sum;
}

uint8_t* copy_image(const uint8_t* src, size_t size)
{
    uint8_t* buf = malloc(size);
    EXPECT_TRUE(buf != NULL);
    memcpy(buf, src, size);
    return buf;
}

/* copy, patch [off, off+len) with val, return the mutated copy */
uint8_t* mutated_copy(const uint8_t* src, size_t size, size_t off,
                      const void* val, size_t len)
{
    uint8_t* buf = copy_image(src, size);
    memcpy(buf + off, val, len);
    return buf;
}

/* Write a 12-bit FAT entry (1.5-byte little-endian packing) into the first
 * FAT table of the raw image. */
void set_fat12_entry(uint8_t* img, uint32_t cluster, uint16_t value)
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
void set_fat32_entry(uint8_t* img, uint32_t cluster, uint32_t value)
{
    size_t off = F32_FAT_SECTOR * 512u + (size_t)cluster * 4u;
    img[off] = (uint8_t)(value & 0xFFu);
    img[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    img[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    img[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

/* open_mem must fail; a context accidentally bound on failure is released */
void assert_open_mem_fails(const uint8_t* img, size_t size, fat_result_t want)
{
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), want);
    if (ctx != NULL)
        fat_close(ctx);
}

/* ------------------------------------------------------------------ */
/* mtools oracle                                                       */
/* ------------------------------------------------------------------ */

/* mtools 4.0.48 `mdir -b` format, verified empirically on this device:
 * one entry per line as "::/dir/name" -- the requested directory is
 * repeated in each line, directories carry a trailing '/', names are
 * printed in lowercase (we render uppercase), and neither the volume
 * label nor "."/".." entries are listed.  No sizes are printed; content
 * equality is covered by mtype instead. */

static int mtools_probe_state = -1;

int mtools_present(void)
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

int upper_eq(const char* a, const char* b)
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
int run_mdir_b(const char* img_name, const char* dir, MDirEntry* out, int max)
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

/* the library's listing of `cluster` must equal mdir's listing of `dir` */
void assert_listing_matches_mdir(fat_ctx_t* ctx, const char* img_name,
                                 const char* dir, uint32_t cluster)
{
    MDirEntry theirs[MDIR_MAX];
    int tn = run_mdir_b(img_name, dir, theirs, MDIR_MAX);
    if (tn < 0)
        fprintf(stderr, "mdir failed: %s ::%s\n", img_name, dir);
    EXPECT_TRUE(tn >= 0);

    OurListing ours;
    memset(&ours, 0, sizeof(ours));
    EXPECT_EQ(iter_dir(ctx, cluster, list_ours_cb, &ours), FAT_OK);

    if (ours.n != tn)
        fprintf(stderr, "listing size mismatch for ::%s: ours %d mdir %d\n",
                dir, ours.n, tn);
    EXPECT_EQ(ours.n, tn);
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
                EXPECT_TRUE(!found); /* names are unique: at most one match */
                EXPECT_EQ(theirs[i].is_dir, ours.e[j].is_dir);
                found = 1;
            }
        }
        if (!found)
            fprintf(stderr, "mdir entry not in our listing: %s%s\n",
                    theirs[i].name, theirs[i].is_dir ? "/" : "");
        EXPECT_TRUE(found);
    }
}

/* byte-compare `mtype -i img ::path` with fat_read_file */
void assert_mtype_matches_read(fat_ctx_t* ctx, const char* img_name,
                               const char* path)
{
    char cmd[256];
    /* quoted: LFN paths contain spaces (lead adjudication 2026-10-06 --
     * unquoted, sh word-splitting broke mtype on long names) */
    snprintf(cmd, sizeof(cmd), "mtype -i \"%s\" ::\"%s\" 2>/dev/null", img_name,
             path);
    FILE* p = popen(cmd, "r");
    if (p == NULL)
        perror("mtype");
    EXPECT_TRUE(p != NULL);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de), FAT_OK);
    uint8_t* want = NULL;
    size_t want_size = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &want, &want_size), FAT_OK);

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
    EXPECT_EQ(status, 0);
    EXPECT_TRUE(!mismatch);
    EXPECT_EQ(got_total, want_size);
    free(want);
}

/* ------------------------------------------------------------------ */
/* content checks                                                      */
/* ------------------------------------------------------------------ */

/* fat_lookup + fat_read_file must deliver exactly `want` */
void assert_read_string(fat_ctx_t* ctx, const char* path, const char* want)
{
    static uint8_t sentinel; /* non-NULL start catches "OK but out unwritten" */
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    size_t want_size = strlen(want);

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de), FAT_OK);
    EXPECT_EQ(de.file_size, want_size);
    uint8_t* data = &sentinel;
    size_t size = 1;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_OK);
    EXPECT_TRUE(data != &sentinel);
    EXPECT_EQ(size, want_size);
    EXPECT_MEMEQ(data, want, want_size);
    free(data);
}

/* ... exactly the first want_size bytes of the host file */
void assert_read_matches_host(fat_ctx_t* ctx, const char* path,
                              const char* host_file, size_t want_size)
{
    static uint8_t sentinel;
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &de), FAT_OK);
    EXPECT_EQ(de.file_size, want_size);
    uint8_t* data = &sentinel;
    size_t size = 1;
    EXPECT_EQ(fat_read_file(ctx, &de, &data, &size), FAT_OK);
    EXPECT_TRUE(data != &sentinel);
    EXPECT_EQ(size, want_size);
    {
        FILE* fp = fopen(host_file, "rb");
        if (fp == NULL)
            perror(host_file);
        EXPECT_TRUE(fp != NULL);
        uint8_t* expected = malloc(size);
        EXPECT_TRUE(expected != NULL);
        EXPECT_EQ(fread(expected, 1, size, fp), size);
        fclose(fp);
        EXPECT_MEMEQ(data, expected, size);
        free(expected);
    }
    free(data);
}

/* ------------------------------------------------------------------ */
/* directory and FAT statistics                                        */
/* ------------------------------------------------------------------ */

static void count_any_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    (void)entry;
    (void)raw32;
    (*(int*)user_data)++;
}

/* live entry count of a directory (label and dot entries included) */
int count_dir_entries(fat_ctx_t* ctx, uint32_t cluster)
{
    int n = 0;
    EXPECT_EQ(iter_dir(ctx, cluster, count_any_cb, &n), FAT_OK);
    return n;
}

/* data clusters in the chain headed at `first` (stops at EOC/broken) */
uint32_t chain_length(const fat_ctx_t* ctx, uint32_t first)
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
uint32_t count_free_clusters(const fat_ctx_t* ctx)
{
    uint32_t n = 0;
    for (uint32_t c = 2u; c <= fat_geometry(ctx)->cluster_count + 1u; c++)
        if (fat_at(ctx, c) == 0)
            n++;
    return n;
}

/* deterministic write pattern */
uint8_t* make_pattern(size_t n)
{
    uint8_t* p = malloc(n);
    EXPECT_TRUE(p != NULL);
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 7u + 3u);
    return p;
}

/* ------------------------------------------------------------------ */
/* write round trips                                                   */
/* ------------------------------------------------------------------ */

/* After writing `name` into `dir_cluster`: lookup finds it, the dirent
 * size/first_cluster are exact, the FAT chain holds `want_clusters` data
 * clusters, and both fat_read_file and a fat_file_t cursor deliver the
 * original bytes. */
void assert_write_roundtrip(fat_ctx_t* ctx, uint32_t dir_cluster,
                            const char* name, const uint8_t* data, size_t size,
                            uint32_t want_clusters)
{
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, dir_cluster, name, &de), FAT_OK);
    EXPECT_EQ(de.file_size, size);
    if (size == 0) {
        EXPECT_EQ(de.first_cluster, 0); /* empty file: no chain at all */
        return;
    }
    EXPECT_TRUE(de.first_cluster >= 2u);
    EXPECT_TRUE(de.first_cluster <= fat_geometry(ctx)->cluster_count + 1u);
    EXPECT_EQ(chain_length(ctx, de.first_cluster), want_clusters);

    uint8_t* back = NULL;
    size_t back_size = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &back, &back_size), FAT_OK);
    EXPECT_EQ(back_size, size);
    EXPECT_MEMEQ(back, data, size);
    free(back);

    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open(ctx, &de, &f), FAT_OK);
    uint8_t* buf = malloc(size);
    EXPECT_TRUE(buf != NULL);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(f, buf, size, &got), FAT_OK);
    EXPECT_EQ(got, size);
    EXPECT_MEMEQ(buf, data, size);
    fat_file_close(f);
    free(buf);
}

/* every geometry field must match (reopen comparisons) */
void assert_same_geometry(const fat_geometry_t* a, const fat_geometry_t* b)
{
    EXPECT_EQ(a->bytes_per_sector, b->bytes_per_sector);
    EXPECT_EQ(a->sectors_per_cluster, b->sectors_per_cluster);
    EXPECT_EQ(a->reserved_sectors, b->reserved_sectors);
    EXPECT_EQ(a->fat_count, b->fat_count);
    EXPECT_EQ(a->fat_sectors, b->fat_sectors);
    EXPECT_EQ(a->root_entries, b->root_entries);
    EXPECT_EQ(a->total_sectors, b->total_sectors);
    EXPECT_EQ(a->fat_start_sector, b->fat_start_sector);
    EXPECT_EQ(a->root_dir_sector, b->root_dir_sector);
    EXPECT_EQ(a->root_dir_sectors, b->root_dir_sectors);
    EXPECT_EQ(a->root_cluster, b->root_cluster);
    EXPECT_EQ(a->data_start_sector, b->data_start_sector);
    EXPECT_EQ(a->cluster_count, b->cluster_count);
}

void write_image(const char* name, const uint8_t* buf, size_t size)
{
    FILE* fp = fopen(name, "wb");
    if (fp == NULL)
        perror(name);
    EXPECT_TRUE(fp != NULL);
    EXPECT_EQ(fwrite(buf, 1, size, fp), size);
    fclose(fp);
}
