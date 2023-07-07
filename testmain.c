#include "fat.h"
#include <assert.h>
#include <string.h>

void test_fat_init(FILE* fp) {
    bool ret = fat_init(fp);
    FatBS* bs = (FatBS*)fat_get_ptr();
    assert(ret);
    assert(bs->bootJmp[0] == 0xeb);
    assert(bs->bootJmp[1] == 0x3c);
    assert(bs->bootJmp[2] == 0x90);
    // The volume name depends on the mtool version you use
    assert(strncmp((const char*)bs->oemName, "MTOO4032", 8) == 0);
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

void test_subdirs1() {
    DirectoryEntry* directoryEntries;
    FatBS* bs = (FatBS*)fat_get_ptr();

    directoryEntries =
        (DirectoryEntry*)fat_get_root_directory_start_sector_ptr();
    int subdirs = 0;
    int files = 0;

    for (int i = 0; i < bs->rootEntryCount; i++) {
        DirectoryEntry* entry = &directoryEntries[i];
        // 0x00 not use, 0xE5 deleted
        if (entry->name[0] == 0x00 || entry->name[0] == 0xE5)
            break;
        if (entry->attributes & 0x10) {
            if (entry->name[0] != '.') {
                // Directory
                subdirs++;
            }
        } else {
            files++;
        }
    }
    assert(subdirs == 2);
    assert(files == 3);
}

void test_subdirs2() {
    uint32_t cluster = 11;
    uint32_t nextCluster;
    DirectoryEntry* directoryEntries;
    FatBS* bs = (FatBS*)fat_get_ptr();

    nextCluster = fat_get_fat(cluster);

    int subdirs = 0;
    int files = 0;
    while (cluster < 0xF00) {
        nextCluster = fat_get_fat(cluster);
        directoryEntries = (DirectoryEntry*)(fat_get_cluster_ptr(cluster));
        for (int i = 0; i < bs->bytesPerSector * bs->sectorsPerCluster /
                                sizeof(DirectoryEntry);
             i++) {
            DirectoryEntry* entry = &directoryEntries[i];
            // 0x00 not use, 0xE5 deleted
            if (entry->name[0] == 0x00 || entry->name[0] == 0xE5)
                break;
            if (entry->attributes & 0x10) {
                if (entry->name[0] != '.') {
                    // Directory
                    subdirs++;
                }
            } else {
                files++;
            }
        }
        cluster = nextCluster;
    }
    assert(subdirs == 33);
    assert(files == 0);
}

void test_fat_set_entry_name() {
    DirectoryEntry entry;

    fat_set_entry_name(&entry, "hoge");
    assert(memcmp(entry.name, "HOGE       ", 11) == 0);

    fat_set_entry_name(&entry, "page.txt");
    assert(memcmp(entry.name, "PAGE    TXT", 11) == 0);

    fat_set_entry_name(&entry, "foo.a");
    assert(memcmp(entry.name, "FOO     A  ", 11) == 0);
}

void test_fat_get_cluster_for_entry() {
    DirectoryEntry entry;
    fat_set_entry_name(&entry, "           ");
    uint32_t cluster = fat_get_cluster_for_entry(0, &entry);
    assert(cluster == 0);

    fat_set_entry_name(&entry, "DIR1");
    cluster = fat_get_cluster_for_entry(0, &entry);
    assert(cluster == 8);

    fat_set_entry_name(&entry, "dir2");
    cluster = fat_get_cluster_for_entry(0, &entry);
    assert(cluster == 11);
}

int main() {
    FILE* fp = fopen("demof12.fat", "rb");
    assert(fp != NULL);

    test_fat_init(fp);
    test_fat_get_sector_ptr();
    test_fat_get_fat();
    test_fat_get_root_directory_start_sector_ptr();
    test_subdirs1();
    test_subdirs2();
    test_fat_set_entry_name();
    test_fat_get_cluster_for_entry();
    fat_unint();

    char* token;
    char str[] = "dir1/dir2/file";
    const char* delim = "/";
    token = strtok(str, delim);
    while (token) {
        printf("token: %s\n", token);
        token = strtok(NULL, delim);
    }

    fclose(fp);
    return 0;
}
