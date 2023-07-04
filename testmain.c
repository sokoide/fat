#include "fat.h"
#include <assert.h>
#include <string.h>

void test_fat_init_info(FILE* fp) {
    FatBS bs;
    bool ret = fat_init_info(fp, &bs);
    assert(ret);
    assert(bs.bootJmp[0] == 0xeb);
    assert(bs.bootJmp[1] == 0x3c);
    assert(bs.bootJmp[2] == 0x90);
    // The volume name depends on the mtool version you use
    assert(strncmp((const char*)bs.oemName, "MTOO4032", 8) == 0);
    assert(bs.bytesPerSector == 512);
    assert(bs.sectorsPerCluster == 2);
    assert(bs.reservedSectorCount == 1);
    assert(bs.tableCount == 2);
    assert(bs.rootEntryCount == 0x70);
    assert(bs.totalSectors16 == 0x5a0);
    assert(bs.mediaType == 0xf9);
    assert(bs.tableSize16 == 3);
}

void test_fat_get_sector(FILE* fp) {
    FatBS bs;
    bool ret = fat_init_info(fp, &bs);
    assert(ret);

    uint8_t sector[512];
    ret =
        fat_get_sector(fp, &bs, 0, sector, sizeof(sector) / sizeof(sector[0]));
    assert(ret);
    assert(sector[0] == 0xeb);

    ret =
        fat_get_sector(fp, &bs, 1, sector, sizeof(sector) / sizeof(sector[0]));
    assert(ret);
    assert(sector[0] == 0xf9);
    assert(sector[1] == 0xff);
}

int main() {
    FILE* fp = fopen("demof12.fat", "rb");
    assert(fp != NULL);

    test_fat_init_info(fp);
    test_fat_get_sector(fp);

    fclose(fp);
    return 0;
}
