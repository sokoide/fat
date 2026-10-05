#include "fat.h"
#include <assert.h>
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

typedef struct {
    int subdirs;
    int files;
} DirCounts;

/* Skip volume labels (0x08), LFN entries (0x0F) and '.' / '..' so the counts
 * match what mdir -b reports for the same directory. */
static void count_entries(DirectoryEntry* entry, void* p) {
    DirCounts* counts = (DirCounts*)p;
    if ((entry->attributes & 0x08) != 0 || entry->attributes == 0x0F)
        return;
    if (entry->name[0] == '.')
        return;
    if (entry->attributes & 0x10)
        counts->subdirs++;
    else
        counts->files++;
}

void test_fat_init() {
    FILE* fp = fopen("demof12.fat", "rb");
    if (fp == NULL)
        perror("demof12.fat");
    assert(fp != NULL);

    bool ret = fat_init(fp);
    fclose(fp);
    FatBS* bs = (FatBS*)fat_get_ptr();
    assert(ret);
    assert(bs->bootJmp[0] == 0xeb);
    assert(bs->bootJmp[1] == 0x3c);
    assert(bs->bootJmp[2] == 0x90);
    // The OEM name and volume label depend on the mtools version; the
    // geometry assertions below cover what the driver actually relies on.
    assert(bs->bytesPerSector == 512);
    assert(bs->sectorsPerCluster == 2);
    assert(bs->reservedSectorCount == 1);
    assert(bs->tableCount == 2);
    assert(bs->rootEntryCount == 0x70);
    assert(bs->totalSectors16 == 0x5a0);
    assert(bs->mediaType == 0xf9);
    assert(bs->tableSize16 == 3);
}

void test_fat_get_sector_ptr() {
    uint8_t* sector = fat_get_sector_ptr(0);
    assert(sector[0] == 0xeb);

    sector = fat_get_sector_ptr(1);
    assert(sector[0] == 0xf9);
    assert(sector[1] == 0xff);
}

void test_fat_get_fat() {
    uint32_t fat = fat_get_fat(0);
    assert(fat == 0xFF9);
    fat = fat_get_fat(1);
    assert(fat == 0xFFF);
    fat = fat_get_fat(2);
    assert(fat == 0xFFF);
    fat = fat_get_fat(3);
    assert(fat == 0x004);
    fat = fat_get_fat(4);
    assert(fat == 0x005);
    fat = fat_get_fat(5);
    assert(fat == 0x006);
    fat = fat_get_fat(6);
    assert(fat == 0x007);
    fat = fat_get_fat(7);
    assert(fat == 0xFFF);
    fat = fat_get_fat(8);
    assert(fat == 0xFFF);
    fat = fat_get_fat(11);
    assert(fat == 0x02B);
}

void test_fat_get_root_directory_start_sector_ptr() {
    uint8_t* p = fat_get_root_directory_start_sector_ptr();
    uint8_t* buffer = fat_get_ptr();
    assert(p == buffer + 512 * 7);
}

/* Upper bound on chain length: the image cannot hold more data clusters than
 * totalSectors / sectorsPerCluster. Guards against FAT cycles. */
static uint32_t max_chain_length() {
    FatBS* bs = (FatBS*)fat_get_ptr();
    return bs->totalSectors16 / bs->sectorsPerCluster + 1;
}

void test_subdirs1() {
    DirCounts counts = {0, 0};
    iterate_rootdir(count_entries, &counts);
    /* dir1, dir2 / hello.txt, test_5kb.txt (volume label excluded) */
    assert(counts.subdirs == 2);
    assert(counts.files == 2);
}

void test_subdirs2() {
    DirectoryEntry entry;
    memset(&entry, 0, sizeof(entry));
    assert(fat_set_entry_name(&entry, "dir2"));
    uint32_t cluster = fat_get_cluster_for_entry(FAT_CLUSTER_ROOT, &entry);
    assert(cluster != FAT_CLUSTER_NOT_FOUND);

    /* dir2 spans clusters 11 -> 43 -> EOC and holds subdir1..subdir33 */
    DirCounts counts = {0, 0};
    iterate_dir(cluster, count_entries, &counts);
    assert(counts.subdirs == 33);
    assert(counts.files == 0);
}

void test_fat_set_entry_name() {
    DirectoryEntry entry;
    char name[16];

    /* fat_get_entry_name reads attributes; start from a known state */
    memset(&entry, 0, sizeof(entry));

    assert(fat_set_entry_name(&entry, "hoge"));
    assert(memcmp(entry.name, "HOGE       ", 11) == 0);
    assert(fat_get_entry_name(&entry, name, sizeof(name)) != NULL);
    assert(strcmp(name, "HOGE") == 0);

    assert(fat_set_entry_name(&entry, "page.txt"));
    assert(memcmp(entry.name, "PAGE    TXT", 11) == 0);
    assert(fat_get_entry_name(&entry, name, sizeof(name)) != NULL);
    assert(strcmp(name, "PAGE.TXT") == 0);

    /* extension trailing spaces are trimmed, not padded */
    assert(fat_set_entry_name(&entry, "foo.a"));
    assert(memcmp(entry.name, "FOO     A  ", 11) == 0);
    assert(fat_get_entry_name(&entry, name, sizeof(name)) != NULL);
    assert(strcmp(name, "FOO.A") == 0);

    /* extension-less names get no dot */
    assert(fat_set_entry_name(&entry, "readme"));
    assert(memcmp(entry.name, "README      ", 11) == 0);
    assert(fat_get_entry_name(&entry, name, sizeof(name)) != NULL);
    assert(strcmp(name, "README") == 0);

    /* directories (attr 0x10) are formatted the same way, no dot */
    memset(&entry, 0, sizeof(entry));
    entry.attributes = 0x10;
    assert(fat_set_entry_name(&entry, "subdir1"));
    assert(fat_get_entry_name(&entry, name, sizeof(name)) != NULL);
    assert(strcmp(name, "SUBDIR1") == 0);

    /* names not representable in 8.3 are rejected */
    assert(!fat_set_entry_name(&entry, "toolongname.txt"));
    assert(!fat_set_entry_name(&entry, "file.text"));
}

void test_fat_get_cluster_for_entry() {
    DirectoryEntry entry;

    /* blank name matches nothing */
    memset(&entry, 0, sizeof(entry));
    fat_set_entry_name(&entry, "");
    uint32_t cluster = fat_get_cluster_for_entry(FAT_CLUSTER_ROOT, &entry);
    assert(cluster == FAT_CLUSTER_NOT_FOUND);

    fat_set_entry_name(&entry, "DIR1");
    cluster = fat_get_cluster_for_entry(FAT_CLUSTER_ROOT, &entry);
    assert(cluster == 8);

    fat_set_entry_name(&entry, "dir2");
    cluster = fat_get_cluster_for_entry(FAT_CLUSTER_ROOT, &entry);
    assert(cluster == 11);
}

/* Chain-read the multi-cluster root file TEST_5KBTXT (4962 bytes over 5
 * clusters) and compare it byte-for-byte with the repo copy of the file. */
void test_read_multicluster_file() {
    DirectoryEntry entry;
    memset(&entry, 0, sizeof(entry));
    assert(fat_set_entry_name(&entry, "test_5kb.txt"));
    uint32_t cluster = fat_get_cluster_for_entry(FAT_CLUSTER_ROOT, &entry);
    assert(cluster != FAT_CLUSTER_NOT_FOUND);
    assert(cluster >= 2);

    uint32_t clusterSize = fat_get_cluster_size();
    uint32_t size = entry.fileSize;
    assert(size == 4962);
    assert((size + clusterSize - 1) / clusterSize == 5);

    /* the chain ends cleanly at EOC after exactly 5 clusters */
    uint32_t expectedClusters = (size + clusterSize - 1) / clusterSize;
    uint32_t seen = 0;
    uint32_t c = cluster;
    while (c >= 2 && seen < max_chain_length()) {
        seen++;
        if (fat_is_end_of_cluster(c) || fat_is_broken(c))
            break;
        c = fat_get_fat(c);
    }
    assert(seen == expectedClusters);
    assert(fat_is_end_of_cluster(c));

    FILE* fp = fopen("test_5kb.txt", "rb");
    if (fp == NULL)
        perror("test_5kb.txt");
    assert(fp != NULL);
    uint8_t* expected = malloc(size);
    uint8_t* actual = malloc(size);
    assert(expected != NULL && actual != NULL);
    assert(fread(expected, 1, size, fp) == size);
    fclose(fp);

    uint32_t off = 0;
    c = cluster;
    while (off < size && c >= 2) {
        void* data = fat_get_cluster_ptr(c);
        assert(data != NULL);
        uint32_t n = size - off < clusterSize ? size - off : clusterSize;
        memcpy(actual + off, data, n);
        off += n;
        if (fat_is_end_of_cluster(c) || fat_is_broken(c))
            break;
        c = fat_get_fat(c);
    }
    assert(off == size);
    assert(memcmp(actual, expected, size) == 0);

    free(expected);
    free(actual);
}

int main(void) {
    RUN(test_fat_init);
    RUN(test_fat_get_sector_ptr);
    RUN(test_fat_get_fat);
    RUN(test_fat_get_root_directory_start_sector_ptr);
    RUN(test_subdirs1);
    RUN(test_subdirs2);
    RUN(test_fat_set_entry_name);
    RUN(test_fat_get_cluster_for_entry);
    RUN(test_read_multicluster_file);
    fat_uninit();

    return 0;
}
