#include "fat.h"
#include <assert.h>
#include <string.h>

void test_init_fat_info(FILE* fp) {
    FatBS bs;
    bool ret = init_fat_info(fp, &bs);
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

int main() {
    FILE* fp = fopen("demof12.fat", "rb");
    assert(fp != NULL);

    test_init_fat_info(fp);

    fclose(fp);
    return 0;
}
