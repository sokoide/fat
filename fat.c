#include "fat.h"
#include "color.h"
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// functions

bool fat_get_sector(FILE* fp /* in */, FatBS* bs /* in */, int sector /* in */,
                    uint8_t* buffer /* out */, int buffer_size /* in */) {
    uint8_t b[512];
    if (buffer_size < bs->bytesPerSector) {
        fprintf(stderr, "buffer too small.\n");
        return false;
    }

    fseek(fp, sector * bs->bytesPerSector, SEEK_SET);
    size_t read = fread(b, bs->bytesPerSector, 1, fp);
    if (read != 1) {
        fprintf(stderr, "fat_get_sector failed to read.\n");
        return false;
    }

    return memcpy(buffer, b, bs->bytesPerSector) != NULL;
}

bool fat_init_info(FILE* fp /* in */, FatBS* bs /* out */) {
    uint8_t b[512];
    fseek(fp, 0, SEEK_SET);
    size_t read = fread(b, sizeof(b) / sizeof(b[0]), 1, fp);
    if (read != 1) {
        fprintf(stderr, "fat_init_info failed to read.\n");
        return false;
    }

    return memcpy(bs, b, sizeof(FatBS)) != NULL;
}
