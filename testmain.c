#include "fat.h"
#include <assert.h>
#include <string.h>

void test_fat_init(FILE* fp) {
    bool ret = fat_init(fp);
    FatBS* bs = (FatBS*)_fat_buffer;
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

void test_fat_get_sector() {
    uint8_t* sector = fat_get_sector(0);
    assert(sector[0] == 0xeb);

    sector = fat_get_sector(1);
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
}

int main() {
    FILE* fp = fopen("demof12.fat", "rb");
    assert(fp != NULL);

    test_fat_init(fp);
    test_fat_get_sector();
    test_fat_get_fat();
    fat_unint();

    fclose(fp);
    return 0;
}
