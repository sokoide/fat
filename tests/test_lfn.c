// tests/test_lfn.c -- phase 7: LFN (long file names), §20

#include "test_util.h"

/* ------------------------------------------------------------------ */
/* phase 7: LFN (long file names), §20                                 */
/*                                                                     */
/* Fixture facts probed 2026-10-06 (mtools 4.0.43 + od, /tmp/p7probe): */
/*   - demof12 root: slots 0..4 live (label, HELLO.TXT, TEST_5KB.TXT,  */
/*     DIR1, DIR2), slots 5..111 are 0x00                              */
/*   - demof16 root: slots 0..3 live, slot 4.. free (offset 255*512)   */
/*   - demof32 root chain: cluster 55 (sector 1125) holds 12 entries,  */
/*     slots 12..15 are 0x00 (byte offset (1072+53)*512)               */
/*   - mcopy writes "a long name file.txt" as LFN seq2+seq1 plus the   */
/*     8.3 alias ALONGN~1 TXT (checksum 0x42); the exact slot bytes    */
/*     are pinned in test_lfn_put_helpers_vs_mcopy below               */
/*   - mdir -b prints the LFN (not the alias); mtype resolves both     */
/*   - alias mangling: mtools/Windows drop spaces (a long name ->      */
/*     ALONGN~1); tests only pin aliases of space-free ASCII names     */
/*     where the Windows rule is unambiguous                           */
/* ------------------------------------------------------------------ */

#define F12_ROOT_OFF ((size_t)FIXTURE_ROOT_SECTOR * 512u)
#define F16_ROOT_OFF (255u * 512u)
#define F32_CLU55_OFF ((1072u + 53u) * 512u)
#define LFN_MAX_UNITS 256u /* 255 chars + NUL */

/* short-name checksum over the 11 on-disk name bytes (fatgen103) */
static uint8_t lfn_checksum11(const uint8_t name11[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1u) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

/* UTF-8 -> UTF-16 code units (surrogate pair for cp >= 0x10000);
 * returns the unit count, NUL excluded */
static size_t utf8_to_utf16(const char* s, uint16_t out[LFN_MAX_UNITS])
{
    const unsigned char* p = (const unsigned char*)s;
    size_t n = 0;
    while (*p != 0) {
        uint32_t cp;
        int len;
        if (*p < 0x80u) {
            cp = *p;
            len = 1;
        } else if ((*p & 0xE0u) == 0xC0u) {
            cp = *p & 0x1Fu;
            len = 2;
        } else if ((*p & 0xF0u) == 0xE0u) {
            cp = *p & 0x0Fu;
            len = 3;
        } else {
            cp = *p & 0x07u;
            len = 4;
        }
        for (int i = 1; i < len; i++)
            cp = (cp << 6) | (p[i] & 0x3Fu);
        p += len;
        if (cp >= 0x10000u) {
            EXPECT_TRUE(n + 1 < LFN_MAX_UNITS);
            out[n++] = (uint16_t)(0xD800u + ((cp - 0x10000u) >> 10));
            out[n++] = (uint16_t)(0xDC00u + ((cp - 0x10000u) & 0x3FFu));
        } else {
            EXPECT_TRUE(n < LFN_MAX_UNITS);
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

/* synthetic 8.3 slot; `name11` is an exact 11-character string */
static void put_83_slot(uint8_t* img, size_t off, const char* name11,
                        uint8_t attr, uint32_t cluster, uint32_t size)
{
    EXPECT_EQ(strlen(name11), 11);
    uint8_t* e = img + off;
    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = attr;
    e[26] = (uint8_t)(cluster & 0xFFu);
    e[27] = (uint8_t)(cluster >> 8);
    e[28] = (uint8_t)(size & 0xFFu);
    e[29] = (uint8_t)((size >> 8) & 0xFFu);
    e[30] = (uint8_t)((size >> 16) & 0xFFu);
    e[31] = (uint8_t)((size >> 24) & 0xFFu);
}

/* synthetic LFN run for `utf8_name` at byte offset `off`: seq descending,
 * 0x40 on the physical first entry, NUL terminator + 0xFFFF padding,
 * checksum of `name11` in byte 13 of every entry. The caller places the
 * 8.3 slot right after. Returns the number of 32-byte slots used. */
static unsigned put_lfn_run(uint8_t* img, size_t off, const char* utf8_name,
                            const uint8_t name11[11])
{
    uint16_t units[LFN_MAX_UNITS];
    size_t n = utf8_to_utf16(utf8_name, units);
    EXPECT_TRUE(n <= 255u);
    units[n++] = 0; /* terminator */
    unsigned entries = (unsigned)((n + 12u) / 13u);
    EXPECT_TRUE(entries <= 20u);
    uint8_t csum = lfn_checksum11(name11);
    static const int unit_off[13] = {1,  3,  5,  7,  9,  14, 16,
                                     18, 20, 22, 24, 28, 30};
    for (unsigned e = 0; e < entries; e++) {
        unsigned seq = entries - e; /* physical order: seq N first */
        uint8_t* slot = img + off + (size_t)e * 32u;
        memset(slot, 0, 32);
        slot[0] = (uint8_t)(seq | (e == 0 ? 0x40u : 0u));
        slot[11] = 0x0F;
        slot[12] = 0;
        slot[13] = csum;
        size_t base = (size_t)(seq - 1u) * 13u;
        for (int i = 0; i < 13; i++) {
            size_t u = base + (size_t)i;
            uint16_t v = u < n ? units[u] : 0xFFFFu;
            slot[unit_off[i]] = (uint8_t)(v & 0xFFu);
            slot[unit_off[i] + 1] = (uint8_t)(v >> 8);
        }
    }
    return entries;
}

/* stepwise-cursor search: 1 + dirent copy when a live entry renders
 * exactly as `name` (LFN preferred, 8.3 fallback) */
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

typedef struct {
    const char* want;
    int found;
} LfnNameProbe;

static void lfn_probe_cb(const fat_dirent_t* entry, const uint8_t* raw32,
                         void* user_data)
{
    (void)raw32;
    LfnNameProbe* p = user_data;
    if (strcmp(entry->name, p->want) == 0)
        p->found = 1;
}

/* 7.0: the synthetic-entry helpers reproduce the exact bytes mtools
 * 4.0.43 mcopy writes for "a long name file.txt" (od-probed), and the
 * checksum formula matches the on-disk value. Locks every other LFN
 * test to a verified ground truth. Green by design (no lib calls). */
static void test_lfn_put_helpers_vs_mcopy(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);

    static const char alias11[12] = "ALONGN~1TXT";
    EXPECT_EQ(lfn_checksum11((const uint8_t*)alias11), 0x42);
    EXPECT_EQ(put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                          (const uint8_t*)alias11),
              2);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, alias11, 0x20, 2, 12);

    /* slot bytes exactly as mcopy wrote them (seq 2, padding after NUL) */
    static const uint8_t want_seq2[32] = {
        0x42, 0x69, 0x00, 0x6c, 0x00, 0x65, 0x00, 0x2e, 0x00, 0x74, 0x00,
        0x0f, 0x00, 0x42, 0x78, 0x00, 0x74, 0x00, 0x00, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t want_seq1[32] = {
        0x01, 0x61, 0x00, 0x20, 0x00, 0x6c, 0x00, 0x6f, 0x00, 0x6e, 0x00,
        0x0f, 0x00, 0x42, 0x67, 0x00, 0x20, 0x00, 0x6e, 0x00, 0x61, 0x00,
        0x6d, 0x00, 0x65, 0x00, 0x00, 0x00, 0x20, 0x00, 0x66, 0x00};
    EXPECT_MEMEQ(img + F12_ROOT_OFF + 5u * 32u, want_seq2, 32);
    EXPECT_MEMEQ(img + F12_ROOT_OFF + 6u * 32u, want_seq1, 32);

    /* 8.3 slot: name/attr/cluster/size placed (timestamps stay zero) */
    const uint8_t* s83 = img + F12_ROOT_OFF + 7u * 32u;
    EXPECT_MEMEQ(s83, "ALONGN~1TXT", 11);
    EXPECT_EQ(s83[11], 0x20);
    EXPECT_TRUE(s83[26] == 2 && s83[27] == 0);
    EXPECT_TRUE(s83[28] == 12 && s83[29] == 0);

    /* helper round trip: encode units, decode length matches */
    uint16_t units[LFN_MAX_UNITS];
    EXPECT_EQ(utf8_to_utf16("a long name file.txt", units), 20);
    EXPECT_TRUE(units[0] == 'a' && units[1] == ' ');
    free(img);
}

/* 20.2: a checksum-valid one-entry LFN replaces the 8.3 rendering in
 * dirent->name; attributes, cluster, size still come from the 8.3 slot
 * and raw32 hands out the 8.3 entry's 32 bytes. */
static void test_lfn_read_single(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ONELONG NAM"; /* ONELONG.NAM */
    EXPECT_EQ(put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "onelong.name",
                          (const uint8_t*)alias11),
              1);
    put_83_slot(img, F12_ROOT_OFF + 6u * 32u, alias11, 0x20, 2, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    fat_dir_t* d = NULL;
    EXPECT_EQ(fat_dir_open(ctx, FAT_CLUSTER_ROOT, &d), FAT_OK);
    int seen = 0;
    for (;;) {
        const fat_dirent_t* e = NULL;
        const uint8_t* raw32 = NULL;
        fat_result_t r = fat_dir_next(d, &e, &raw32);
        EXPECT_TRUE(r == FAT_OK || r == FAT_ERR_END_OF_DIR);
        if (r != FAT_OK)
            break;
        if (strcmp(e->name, "onelong.name") == 0) {
            seen = 1;
            EXPECT_EQ(e->attributes, 0x20);
            EXPECT_EQ(e->first_cluster, 2);
            EXPECT_EQ(e->file_size, 12);
            EXPECT_TRUE(raw32 != NULL); /* the 8.3 slot, never the LFN entry */
            EXPECT_MEMEQ(raw32, "ONELONG NAM", 11);
            EXPECT_EQ(raw32[11], 0x20);
        }
    }
    fat_dir_close(d);
    EXPECT_TRUE(seen); /* RED: lib renders the 8.3 alias, not the LFN */

    /* same joining through the callback iterator */
    LfnNameProbe probe = {"onelong.name", 0};
    EXPECT_EQ(iter_dir(ctx, FAT_CLUSTER_ROOT, lfn_probe_cb, &probe),
              FAT_OK);
    EXPECT_TRUE(probe.found);

    /* the LFN entry points at hello's cluster: content reads through */
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "onelong.name", &de));
    uint8_t* buf = NULL;
    size_t n = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &buf, &n), FAT_OK);
    EXPECT_EQ(n, 12);
    EXPECT_MEMEQ(buf, "hello world\n", 12);
    free(buf);
    fat_close(ctx);
}

/* 20.2: two-entry run (the mcopy layout), joined in sequence order */
static void test_lfn_read_two_entries(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ALONGN~1TXT";
    EXPECT_EQ(put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                          (const uint8_t*)alias11),
              2);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, alias11, 0x20, 2, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "a long name file.txt", &de));
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(de.attributes, 0x20);
    /* the 8.3 rendering must not be the joined name anywhere */
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "ALONGN~1.TXT", &de) ||
                strcmp(de.name, "a long name file.txt") == 0);
    fat_close(ctx);
}

/* 20.2: the 255-character maximum (20 LFN entries + the 8.3 slot) */
static void test_lfn_read_max_20_entries(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    char name[256];
    for (int i = 0; i < 255; i++)
        name[i] = (char)('a' + i % 26);
    name[255] = '\0';
    static const char alias11[12] = "MAXNAME TXT";
    EXPECT_EQ(put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, name,
                          (const uint8_t*)alias11),
              20);
    put_83_slot(img, F12_ROOT_OFF + 25u * 32u, alias11, 0x20, 0, 0);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    /* 255 UTF-8 bytes */
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, name, &de));
    EXPECT_EQ(de.file_size, 0);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 6);
    fat_close(ctx);
}

/* 20.2: multibyte (Japanese) LFN, joined and re-encoded as UTF-8 */
static void test_lfn_read_japanese(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "NIPPON  TXT";
    EXPECT_EQ(put_lfn_run(img, F12_ROOT_OFF + 5u * 32u,
                          "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x95\xE3"
                          "\x82\xA1\xE3\x82\xA4\xE3\x83\xAB.txt",
                          (const uint8_t*)alias11),
              1);
    put_83_slot(img, F12_ROOT_OFF + 6u * 32u, alias11, 0x20, 2, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT,
                         "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x95\xE3"
                         "\x82\xA1\xE3\x82\xA4\xE3\x83\xAB.txt",
                         &de));
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(de.file_size, 12);
    fat_close(ctx);
}

/* 20.2: surrogate pair (U+1F389) round-trips UTF-16 -> UTF-8 */
static void test_lfn_read_surrogate_pair(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    uint16_t units[LFN_MAX_UNITS];
    const char* name = "emoji \xF0\x9F\x8E\x89 name.txt"; /* 17 units */
    size_t n = utf8_to_utf16(name, units);
    EXPECT_EQ(n, 17);
    EXPECT_TRUE(units[6] == 0xD83C && units[7] == 0xDF89);
    static const char alias11[12] = "EMOJI_~1TXT";
    EXPECT_EQ(put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, name,
                          (const uint8_t*)alias11),
              2);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, alias11, 0x20, 2, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, name, &de));
    EXPECT_EQ(de.first_cluster, 2);
    fat_close(ctx);
}

/* 20.2: checksum mismatch -> 8.3 fallback (the LFN run is ignored) */
static void test_lfn_read_bad_checksum(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "LONGNAMETXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "longnames.txt",
                (const uint8_t*)alias11);
    put_83_slot(img, F12_ROOT_OFF + 6u * 32u, alias11, 0x20, 2, 12);
    img[F12_ROOT_OFF + 5u * 32u + 13u] ^= 0xFF; /* break the checksum */

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "longnames.txt", &de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "LONGNAME.TXT", &de)); /* 8.3 */
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 6);
    fat_close(ctx);
}

/* 20.2: discontinuous sequence -> fallback */
static void test_lfn_read_seq_disconnected(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ALONGN~1TXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                (const uint8_t*)alias11);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, alias11, 0x20, 2, 12);
    img[F12_ROOT_OFF + 6u * 32u] = 0x03; /* seq 3 where 1 is expected */

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "a long name file.txt", &de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "ALONGN~1.TXT", &de));
    fat_close(ctx);
}

/* 20.2: missing 0x40 flag on the physical first entry -> fallback */
static void test_lfn_read_missing_40_flag(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ALONGN~1TXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                (const uint8_t*)alias11);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, alias11, 0x20, 2, 12);
    img[F12_ROOT_OFF + 5u * 32u] = 0x02; /* 0x42 without the 0x40 flag */

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "a long name file.txt", &de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "ALONGN~1.TXT", &de));
    fat_close(ctx);
}

/* 20.2: orphaned LFN (0x00 follows, no 8.3 entry) never surfaces */
static void test_lfn_read_orphan_no_follower(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ORPHANT TXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "orphaned lfn.txt",
                (const uint8_t*)alias11); /* slot 6 stays 0x00: no follower */

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "orphaned lfn.txt", &de));
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "ORPHANT.TXT", &de));
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 5); /* unchanged */
    fat_close(ctx);
}

/* 20.2: a 0xE5 slot inside the run resets the accumulation -> the
 * surviving fragment falls back to the 8.3 name */
static void test_lfn_read_e5_gap(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ALONGN~1TXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                (const uint8_t*)alias11);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, alias11, 0x20, 2, 12);
    img[F12_ROOT_OFF + 5u * 32u] = 0xE5; /* delete the seq-2 entry */

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(!dir_find(ctx, FAT_CLUSTER_ROOT, "a long name file.txt", &de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "ALONGN~1.TXT", &de));
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 6);
    fat_close(ctx);
}

/* 20.3: lookup resolves the LFN (ASCII case-insensitive) and the 8.3
 * alias to the same entry; an LFN-named directory works as an
 * intermediate path component. */
static void test_lfn_lookup_names(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char file11[12] = "ALONGN~1TXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                (const uint8_t*)file11);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, file11, 0x20, 2, 12);
    /* LFN directory pointing at dir1/subdir1 (cluster 9: only "." "..") */
    static const char dir11[12] = "LONGDI~1   ";
    put_lfn_run(img, F12_ROOT_OFF + 8u * 32u, "long directory name",
                (const uint8_t*)dir11);
    put_83_slot(img, F12_ROOT_OFF + 10u * 32u, dir11, 0x10, 9, 0);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "a long name file.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(de.file_size, 12);
    /* every name-taking API compares ASCII case-insensitively */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "A LONG NAME file.TXT", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    /* the 8.3 alias is the same entry */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "ALONGN~1.TXT", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "alongn~1.txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    /* LFN directory as intermediate component: "." is itself (cluster 9),
     * ".." is dir1 (cluster 8) per subdir1's dot entries */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "long directory name/.", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 9);
    EXPECT_TRUE((de.attributes & 0x10) != 0);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "long directory name/..", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 8);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "no such long name.txt", &de),
              FAT_ERR_NOT_FOUND);
    fat_close(ctx);
}

/* 20.3 + 20.5: open_write/truncate/write through an LFN name, then
 * unlink by the LFN name and rmdir an LFN-named empty directory */
static void test_lfn_open_write_unlink_rmdir(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char file11[12] = "ALONGN~1TXT";
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "a long name file.txt",
                (const uint8_t*)file11);
    put_83_slot(img, F12_ROOT_OFF + 7u * 32u, file11, 0x20, 2, 12);
    static const char dir11[12] = "LONGDI~1   ";
    put_lfn_run(img, F12_ROOT_OFF + 8u * 32u, "long directory name",
                (const uint8_t*)dir11);
    put_83_slot(img, F12_ROOT_OFF + 10u * 32u, dir11, 0x10, 9, 0);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));

    /* write cursor bound by LFN name (aliases the 8.3 slot) */
    fat_file_t* f = NULL;
    EXPECT_EQ(fat_file_open_write(ctx, FAT_CLUSTER_ROOT, "a long name file.txt",
                                  &f),
              FAT_OK);
    EXPECT_EQ(fat_file_truncate(f, 5), FAT_OK); /* "hello world\n" -> "hello" */
    EXPECT_EQ(fat_file_seek(f, 0), FAT_OK);
    size_t w = 0x5A5A;
    EXPECT_EQ(fat_file_write(f, (const uint8_t*)"XYZ", 3, &w), FAT_OK);
    EXPECT_EQ(w, 3);
    fat_file_close(f);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "A LONG NAME FILE.TXT", &de),
              FAT_OK);
    EXPECT_EQ(de.file_size, 5);
    uint8_t buf[8] = {0};
    fat_file_t* rd = NULL;
    EXPECT_EQ(fat_file_open(ctx, &de, &rd), FAT_OK);
    size_t got = 0;
    EXPECT_EQ(fat_file_read(rd, buf, 5, &got), FAT_OK);
    EXPECT_TRUE(got == 5 && memcmp(buf, "XYZlo", 5) == 0);
    fat_file_close(rd);

    /* unlink by LFN name kills both renderings; the freed cluster is
     * hello's (shared), so only slot-level effects are checked here */
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "a long name file.txt"),
              FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "a long name file.txt", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "ALONGN~1.TXT", &de),
              FAT_ERR_NOT_FOUND);

    /* rmdir by LFN name: cluster 9 held only "." and ".." */
    EXPECT_EQ(fat_rmdir(ctx, FAT_CLUSTER_ROOT, "long directory name"),
              FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGDI~1", &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, 9), 0); /* subdir1's chain released */
    fat_close(ctx);
}

/* 20.4: fat_write_file stores an LFN + generated alias; round-trips the
 * name and the bytes, and the on-disk layout is pinned (slots 5..6,
 * checksum of the alias, 0x00 terminator after slot 6). */
static void test_write_file_lfn_roundtrip12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    static const uint8_t data[15] = "hello lfn data";
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt", data, 14,
                             NULL),
              FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "longnames.txt", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, "longnames.txt");
    EXPECT_EQ(de.file_size, 14);
    uint32_t cl = de.first_cluster;
    EXPECT_TRUE(cl >= 2u);
    EXPECT_EQ(chain_length(ctx, cl), 1); /* 14 bytes < 1024 */
    uint8_t* back = NULL;
    size_t back_size = 0;
    EXPECT_EQ(fat_read_file(ctx, &de, &back, &back_size), FAT_OK);
    EXPECT_TRUE(back_size == 14 && memcmp(back, data, 14) == 0);
    free(back);

    /* the alias: LONGNAMES -> 6-char prefix + ~1 = LONGNA~1.TXT */
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGNA~1.TXT", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, cl);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LongNames.Txt", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, cl);

    /* on-disk layout through the export backend: "longnames.txt" is 13
     * chars = 2 LFN slots (seq 2 holds just the terminator) + the 8.3 */
    EXPECT_EQ(fat_write(ctx, "/tmp/p7_lfnw.fat"), FAT_OK);
    FILE* fp = fopen("/tmp/p7_lfnw.fat", "rb");
    EXPECT_TRUE(fp != NULL);
    uint8_t* ex = malloc(size);
    EXPECT_TRUE(ex != NULL);
    EXPECT_EQ(fread(ex, 1, size, fp), size);
    fclose(fp);
    remove("/tmp/p7_lfnw.fat");
    const uint8_t* seq2 = ex + F12_ROOT_OFF + 5u * 32u;
    const uint8_t* seq1 = ex + F12_ROOT_OFF + 6u * 32u;
    const uint8_t* s83 = ex + F12_ROOT_OFF + 7u * 32u;
    static const char alias11[12] = "LONGNA~1TXT";
    /* 0x40|2, terminator only */
    EXPECT_TRUE(seq2[0] == 0x42 && seq2[11] == 0x0F);
    EXPECT_TRUE(seq2[1] == 0 && seq2[2] == 0 && seq2[3] == 0xFF);
    EXPECT_EQ(seq2[13], lfn_checksum11((const uint8_t*)alias11));
    EXPECT_TRUE(seq1[0] == 0x01 && seq1[11] == 0x0F);
    EXPECT_TRUE(seq1[1] == 'l' && seq1[2] == 0 && seq1[3] == 'o' &&
                seq1[4] == 0);
    EXPECT_EQ(seq1[13], lfn_checksum11((const uint8_t*)alias11));
    EXPECT_MEMEQ(s83, alias11, 11);
    EXPECT_EQ(s83[11], 0x20);
    EXPECT_EQ(s83[26], (uint8_t)(cl & 0xFF));
    EXPECT_EQ(s83[27], (uint8_t)(cl >> 8));
    EXPECT_EQ(s83[28], 14);
    EXPECT_EQ(ex[F12_ROOT_OFF + 8u * 32u], 0x00); /* run ends here */
    free(ex);
    fat_close(ctx);
}

/* 20.4: multi-entry names (ASCII 3 slots, Japanese 2 slots) round-trip */
static void test_write_file_lfn_multi_entry(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    static const uint8_t data[11] = "0123456789";
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT,
                             "another long file name.txt", data, 10, NULL),
              FAT_OK);
    const char* jp =
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE9\x95\xB7\xE3"
        "\x81\x84\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB\xE5\x90"
        "\x8D.txt";
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, jp, data, 10, NULL),
              FAT_OK);

    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT,
                           "another long file name.txt", data, 10, 1);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, jp, data, 10, 1);
    fat_close(ctx);
}

/* 20.4: alias collisions number ~1, ~2 (both share the 6-char prefix) */
static void test_write_file_lfn_alias_collision(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longfile1.txt",
                             (const uint8_t*)"one", 3, NULL),
              FAT_OK);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longfile2.txt",
                             (const uint8_t*)"two", 3, NULL),
              FAT_OK);

    fat_dirent_t d1, d2;
    memset(&d1, 0, sizeof(d1));
    memset(&d2, 0, sizeof(d2));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGFI~1.TXT", &d1), FAT_OK);
    EXPECT_STR_EQ(d1.name, "longfile1.txt");
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGFI~2.TXT", &d2), FAT_OK);
    EXPECT_STR_EQ(d2.name, "longfile2.txt");
    EXPECT_TRUE(d1.first_cluster != d2.first_cluster);
    /* both still reachable by their LFN names */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "longfile1.txt", &d1), FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGFILE2.TXT", &d2), FAT_OK);
    fat_close(ctx);
}

/* 20.4: an alias colliding with a live 8.3 name bumps N to ~2 */
static void test_write_file_lfn_alias_vs_existing_83(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longna~1.txt",
                             (const uint8_t*)"A", 1, NULL),
              FAT_OK);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt",
                             (const uint8_t*)"hello lfn data", 14, NULL),
              FAT_OK);

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    /* the plain 8.3 entry keeps ~1 ... Lead adjudication 2026-10-06: the
     * entry has no LFN run, so de.name is the 8.3 rendering (uppercase) */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGNA~1.TXT", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, "LONGNA~1.TXT");
    EXPECT_EQ(de.file_size, 1);
    /* ... so the LFN takes ~2 */
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGNA~2.TXT", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, "longnames.txt");
    EXPECT_EQ(de.file_size, 14);
    fat_close(ctx);
}

/* 20.4: EXISTS fires for the LFN name, its case variants and the alias */
static void test_write_file_lfn_exists(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt",
                             (const uint8_t*)"x", 1, NULL),
              FAT_OK);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt",
                             (const uint8_t*)"y", 1, NULL),
              FAT_ERR_EXISTS);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "LONGNAMES.TXT",
                             (const uint8_t*)"y", 1, NULL),
              FAT_ERR_EXISTS);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "LONGNA~1.TXT",
                             (const uint8_t*)"y", 1, NULL),
              FAT_ERR_EXISTS);
    fat_close(ctx);
}

/* 20.4: name limits -- 255 chars accepted, 256 and '/'/'\' rejected */
static void test_write_file_lfn_name_limits(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    char name[300];
    for (int i = 0; i < 255; i++)
        name[i] = (char)('a' + i % 26);
    name[255] = '\0';
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, name,
                             (const uint8_t*)"x", 1, NULL),
              FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, name, &de), FAT_OK);
    EXPECT_STR_EQ(de.name, name);

    for (int i = 0; i < 256; i++)
        name[i] = (char)('a' + i % 26);
    name[256] = '\0';
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, name,
                             (const uint8_t*)"x", 1, NULL),
              FAT_ERR_NAME_TOO_LONG);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "bad/name.txt",
                             (const uint8_t*)"x", 1, NULL),
              FAT_ERR_NAME_TOO_LONG);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "bad\\name.txt",
                             (const uint8_t*)"x", 1, NULL),
              FAT_ERR_NAME_TOO_LONG);
    fat_close(ctx);
}

/* 20.4: the FAT12 fixed root needs n+1 consecutive slots -- with one
 * slot left an LFN (2 slots) is DIR_FULL while an 8.3 name still fits */
static void test_write_file_lfn_dir_full12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    /* fill slots 5..110 with live dummy 8.3 entries: slot 111 is the
     * only free one left */
    char name11[12];
    for (int i = 0; i < 106; i++) {
        snprintf(name11, sizeof(name11), "FILLR%03dTXT", i + 1);
        put_83_slot(img, F12_ROOT_OFF + (size_t)(5 + i) * 32u, name11, 0x20,
                    0, 0);
    }
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt",
                             (const uint8_t*)"x", 1, NULL),
              FAT_ERR_DIR_FULL);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "zz9.txt",
                             (const uint8_t*)"z", 1, NULL),
              FAT_OK);
    EXPECT_EQ(count_dir_entries(ctx, FAT_CLUSTER_ROOT), 112);
    fat_close(ctx);
}

/* 20.4: slot reuse -- unlinking a 4-slot LFN leaves a 0xE5 run that a
 * later 3-slot LFN must take from the run head (slot 5) */
static void test_write_file_lfn_slot_reuse(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT,
                             "another long file name.txt",
                             (const uint8_t*)"0123456789", 10, NULL),
              FAT_OK);
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT,
                         "another long file name.txt"),
              FAT_OK);
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt",
                             (const uint8_t*)"hello lfn data", 14, NULL),
              FAT_OK);

    EXPECT_EQ(fat_write(ctx, "/tmp/p7_reuse.fat"), FAT_OK);
    FILE* fp = fopen("/tmp/p7_reuse.fat", "rb");
    EXPECT_TRUE(fp != NULL);
    uint8_t* ex = malloc(size);
    EXPECT_TRUE(ex != NULL);
    EXPECT_EQ(fread(ex, 1, size, fp), size);
    fclose(fp);
    remove("/tmp/p7_reuse.fat");
    const uint8_t* seq2 = ex + F12_ROOT_OFF + 5u * 32u;
    const uint8_t* s83 = ex + F12_ROOT_OFF + 7u * 32u;
    EXPECT_TRUE(seq2[0] == 0x42 && seq2[11] == 0x0F); /* reused from run head */
    EXPECT_MEMEQ(s83, "LONGNA~1TXT", 11);
    EXPECT_EQ(ex[F12_ROOT_OFF + 8u * 32u], 0xE5); /* leftover of the run */
    free(ex);
    fat_close(ctx);
}

/* 20.4 non-regression: 8.3-representable names never grow LFN entries */
static void test_write_file_83_no_lfn(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "hello2.txt",
                             (const uint8_t*)"hello world2\n", 13, NULL),
              FAT_OK);
    EXPECT_EQ(fat_write(ctx, "/tmp/p7_83.fat"), FAT_OK);
    FILE* fp = fopen("/tmp/p7_83.fat", "rb");
    EXPECT_TRUE(fp != NULL);
    uint8_t* ex = malloc(size);
    EXPECT_TRUE(ex != NULL);
    EXPECT_EQ(fread(ex, 1, size, fp), size);
    fclose(fp);
    remove("/tmp/p7_83.fat");
    for (unsigned i = 0; i < FIXTURE_ROOT_ENTRIES; i++)
        EXPECT_TRUE(ex[F12_ROOT_OFF + (size_t)i * 32u + 11u] != 0x0F);
    EXPECT_MEMEQ(ex + F12_ROOT_OFF + 5u * 32u, "HELLO2  TXT", 11);
    EXPECT_EQ(ex[F12_ROOT_OFF + 6u * 32u], 0x00); /* exactly one slot */
    free(ex);
    fat_close(ctx);
}

/* 20.5: unlink marks the whole checksum-valid run (LFN entries + the
 * 8.3 slot) 0xE5 at verified byte offsets and frees the data chain;
 * neighbour slots stay byte-identical */
static void test_unlink_lfn_series12(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    static const uint8_t data[20] = "0123456789012345678";
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT,
                             "another long file name.txt", data, 20, NULL),
              FAT_OK);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "another long file name.txt",
                         &de),
              FAT_OK);
    uint32_t cl = de.first_cluster;
    uint32_t free_before = count_free_clusters(ctx);

    /* pre-delete snapshot through the export backend */
    EXPECT_EQ(fat_write(ctx, "/tmp/p7_ul.b.fat"), FAT_OK);
    FILE* fp = fopen("/tmp/p7_ul.b.fat", "rb");
    EXPECT_TRUE(fp != NULL);
    uint8_t* before = malloc(size);
    EXPECT_TRUE(before != NULL);
    EXPECT_EQ(fread(before, 1, size, fp), size);
    fclose(fp);
    remove("/tmp/p7_ul.b.fat");
    /* written layout probed here: 3 LFN slots (5..7) + the 8.3 slot 8 */
    EXPECT_EQ(before[F12_ROOT_OFF + 5u * 32u + 11u], 0x0F);
    EXPECT_EQ(before[F12_ROOT_OFF + 6u * 32u + 11u], 0x0F);
    EXPECT_EQ(before[F12_ROOT_OFF + 7u * 32u + 11u], 0x0F);
    EXPECT_EQ(before[F12_ROOT_OFF + 8u * 32u + 11u], 0x20);
    EXPECT_EQ(before[F12_ROOT_OFF + 9u * 32u], 0x00);

    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "another long file name.txt"),
              FAT_OK);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "another long file name.txt",
                         &de),
              FAT_ERR_NOT_FOUND);
    EXPECT_EQ(fat_at(ctx, cl), 0); /* chain released */
    EXPECT_EQ(count_free_clusters(ctx), free_before + 1u);

    EXPECT_EQ(fat_write(ctx, "/tmp/p7_ul.a.fat"), FAT_OK);
    fp = fopen("/tmp/p7_ul.a.fat", "rb");
    EXPECT_TRUE(fp != NULL);
    uint8_t* after = malloc(size);
    EXPECT_TRUE(after != NULL);
    EXPECT_EQ(fread(after, 1, size, fp), size);
    fclose(fp);
    remove("/tmp/p7_ul.a.fat");
    /* every slot of the run starts with 0xE5 ... */
    for (unsigned i = 5; i <= 8; i++)
        EXPECT_EQ(after[F12_ROOT_OFF + (size_t)i * 32u], 0xE5);
    /* ... and nothing else changed. Lead adjudication 2026-10-06: compare
     * the root slots 0..4 only -- the freed chain legitimately zeroes the
     * FAT entry in both mirrors (asserted above via fat_at), so the
     * original whole-prefix memcmp could never hold */
    EXPECT_MEMEQ(before + F12_ROOT_OFF, after + F12_ROOT_OFF, 5u * 32u);
    EXPECT_MEMEQ(before + F12_ROOT_OFF + 9u * 32u,
                 after + F12_ROOT_OFF + 9u * 32u,
                 size - (F12_ROOT_OFF + 9u * 32u));
    free(before);
    free(after);
    fat_close(ctx);
}

/* 20.5: an orphaned LFN entry (mismatching checksum) directly before
 * the matched 8.3 slot survives the unlink */
static void test_unlink_lfn_orphan_survives(void)
{
    size_t size = 0;
    uint8_t* orig = read_fixture(&size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "ORPHTESTTXT";
    /* slot 5: seq-1 entry whose checksum is broken after the fact (orphan;
     * lead adjudication 2026-10-06: put_lfn_run writes a valid checksum, so
     * it must be corrupted here or slot 5 would be deleted with the 8.3).
     * slots 6-7: the victim's 2-entry LFN run ("victim file.txt" = 13 chars
     * + NUL = 14 units -> 2 entries); slot 8: the 8.3 */
    put_lfn_run(img, F12_ROOT_OFF + 5u * 32u, "orphan a.txt",
                (const uint8_t*)alias11);
    img[F12_ROOT_OFF + 5u * 32u + 13u] ^= 0xFF; /* break the checksum */
    put_lfn_run(img, F12_ROOT_OFF + 6u * 32u, "victim file.txt",
                (const uint8_t*)alias11);
    put_83_slot(img, F12_ROOT_OFF + 8u * 32u, alias11, 0x20, 2, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    EXPECT_EQ(fat_unlink(ctx, FAT_CLUSTER_ROOT, "ORPHTEST.TXT"), FAT_OK);

    EXPECT_EQ(fat_write(ctx, "/tmp/p7_orph.fat"), FAT_OK);
    FILE* fp = fopen("/tmp/p7_orph.fat", "rb");
    EXPECT_TRUE(fp != NULL);
    uint8_t* ex = malloc(size);
    EXPECT_TRUE(ex != NULL);
    EXPECT_EQ(fread(ex, 1, size, fp), size);
    fclose(fp);
    remove("/tmp/p7_orph.fat");
    EXPECT_EQ(ex[F12_ROOT_OFF + 5u * 32u], 0x41); /* orphan stays alive */
    EXPECT_EQ(ex[F12_ROOT_OFF + 5u * 32u + 11u], 0x0F);
    EXPECT_EQ(ex[F12_ROOT_OFF + 6u * 32u], 0xE5); /* checksum-valid LFN */
    EXPECT_EQ(ex[F12_ROOT_OFF + 7u * 32u], 0xE5); /* run (2 entries) ... */
    EXPECT_EQ(ex[F12_ROOT_OFF + 8u * 32u], 0xE5); /* ... and the 8.3 slot */
    free(ex);
    fat_close(ctx);
}

/* 20.2 fixed-root mode on FAT16: synthetic LFN joins + lookups */
static void test_fat16_lfn_read(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG16_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "F16LON~1TXT";
    EXPECT_EQ(put_lfn_run(img, F16_ROOT_OFF + 4u * 32u, "f16 long file.txt",
                          (const uint8_t*)alias11),
              2);
    put_83_slot(img, F16_ROOT_OFF + 6u * 32u, alias11, 0x20, 2, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_TRUE(dir_find(ctx, FAT_CLUSTER_ROOT, "f16 long file.txt", &de));
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "F16 LONG FILE.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "F16LON~1.TXT", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 2);
    fat_close(ctx);
}

/* 20.2 chain mode on FAT32: LFN joining across the root cluster chain
 * (synthetic entry placed in the chain's last cluster, 55) */
static void test_fat32_lfn_read_chain(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    static const char alias11[12] = "F32LON~1TXT";
    EXPECT_EQ(put_lfn_run(img, F32_CLU55_OFF + 12u * 32u, "f32 long file.txt",
                          (const uint8_t*)alias11),
              2);
    put_83_slot(img, F32_CLU55_OFF + 14u * 32u, alias11, 0x20, 3, 12);

    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);
    LfnNameProbe probe = {"f32 long file.txt", 0};
    EXPECT_EQ(iter_dir(ctx, FAT_CLUSTER_ROOT, lfn_probe_cb, &probe),
              FAT_OK);
    EXPECT_TRUE(probe.found);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "f32 long file.txt", &de),
              FAT_OK);
    EXPECT_EQ(de.first_cluster, 3);
    EXPECT_EQ(de.file_size, 12);
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "F32LON~1.TXT", &de), FAT_OK);
    EXPECT_EQ(de.first_cluster, 3);
    fat_close(ctx);
}

/* 20.4 on FAT32: LFN creation lands in the root chain's free tail */
static void test_fat32_write_file_lfn(void)
{
    size_t size = 0;
    uint8_t* orig = read_image(IMG32_NAME, &size);
    uint8_t* img = copy_image(orig, size);
    free(orig);
    fat_ctx_t* ctx = NULL;
    EXPECT_EQ(fat_open_mem(img, size, &ctx), FAT_OK);
    free(img);

    static const uint8_t data[15] = "hello lfn data";
    EXPECT_EQ(fat_write_file(ctx, FAT_CLUSTER_ROOT, "longnames.txt", data, 14,
                             NULL),
              FAT_OK);
    assert_write_roundtrip(ctx, FAT_CLUSTER_ROOT, "longnames.txt", data, 14,
                           1);
    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    EXPECT_EQ(fat_lookup(ctx, FAT_CLUSTER_ROOT, "LONGNA~1.TXT", &de), FAT_OK);
    EXPECT_STR_EQ(de.name, "longnames.txt");
    fat_close(ctx);
}

void test_lfn_register(void)
{
    REGISTER(test_lfn_put_helpers_vs_mcopy);
    REGISTER(test_lfn_read_single);
    REGISTER(test_lfn_read_two_entries);
    REGISTER(test_lfn_read_max_20_entries);
    REGISTER(test_lfn_read_japanese);
    REGISTER(test_lfn_read_surrogate_pair);
    REGISTER(test_lfn_read_bad_checksum);
    REGISTER(test_lfn_read_seq_disconnected);
    REGISTER(test_lfn_read_missing_40_flag);
    REGISTER(test_lfn_read_orphan_no_follower);
    REGISTER(test_lfn_read_e5_gap);
    REGISTER(test_lfn_lookup_names);
    REGISTER(test_lfn_open_write_unlink_rmdir);
    REGISTER(test_write_file_lfn_roundtrip12);
    REGISTER(test_write_file_lfn_multi_entry);
    REGISTER(test_write_file_lfn_alias_collision);
    REGISTER(test_write_file_lfn_alias_vs_existing_83);
    REGISTER(test_write_file_lfn_exists);
    REGISTER(test_write_file_lfn_name_limits);
    REGISTER(test_write_file_lfn_dir_full12);
    REGISTER(test_write_file_lfn_slot_reuse);
    REGISTER(test_write_file_83_no_lfn);
    REGISTER(test_unlink_lfn_series12);
    REGISTER(test_unlink_lfn_orphan_survives);
    REGISTER_AS(test_fat16_lfn_read, NEED_FAT16);
    REGISTER_AS(test_fat32_lfn_read_chain, NEED_FAT32);
    REGISTER_AS(test_fat32_write_file_lfn, NEED_FAT32);
}
