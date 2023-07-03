#include "fat.h"
#include "color.h"
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


// functions

bool init_fat_info(FILE* fp /* in */, FatBS* bs /* out */){
    uint8_t buffer[512];
    size_t read = fread(buffer, sizeof(buffer)/sizeof(uint8_t), 1, fp);
    if (read != 1) {
        fprintf(stderr, "init_fat_info failed to read.\n");
        return false;
    }

    memcpy(bs, buffer, sizeof(FatBS));
    return true;
}
