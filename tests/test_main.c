// test_main.c -- suite entry point: registers every test group (explicit
// calls, no linker magic), brackets the run with the fixture guard (the
// suite must leave the fixture images byte-identical), then runs
// everything with per-test failure isolation (test_util.h).

#include "test_util.h"

/* FNV-1a 64 over the whole image file */
static uint64_t fixture_hash(const char* name)
{
    FILE* fp = fopen(name, "rb");
    if (fp == NULL)
        perror(name);
    EXPECT_TRUE(fp != NULL);
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

/* snapshot the fixtures before the first test */
static void test_fixture_guard_begin(void)
{
    const char* names[3] = {IMG_NAME, IMG16_NAME, IMG32_NAME};
    for (int i = 0; i < 3; i++)
        guard_hash[i] =
            fixture_present(names[i]) ? fixture_hash(names[i]) : 0;
}

/* ... and insist they are unchanged after the last one (end guard) */
static void test_fixture_guard_end(void)
{
    const char* names[3] = {IMG_NAME, IMG16_NAME, IMG32_NAME};
    for (int i = 0; i < 3; i++) {
        if (guard_hash[i] == 0)
            continue; /* absent at start: not guarded (and not written) */
        uint64_t h = fixture_hash(names[i]);
        if (h != guard_hash[i])
            fprintf(stderr, "%s was modified by the test suite\n", names[i]);
        EXPECT_EQ(h, guard_hash[i]);
    }
}

/* the groups (tests/*.c), in run order */
extern void test_read_register(void);
extern void test_io_register(void);
extern void test_write_register(void);
extern void test_lfn_register(void);
extern void test_oracle_register(void);

int main(void)
{
    REGISTER(test_fixture_guard_begin);

    test_read_register();
    test_io_register();
    test_write_register();
    test_lfn_register();
    test_oracle_register();

    REGISTER(test_fixture_guard_end);

    return test_run_all() == 0 ? 0 : 1;
}
