// fat_dev.c -- file-backed image loading; the only stdio user on the
// library side. Never prints: failures propagate as fat_result_t.

#include "fat_internal.h"
#include <stdio.h>
#include <stdlib.h>

fat_result_t fat_open(const char* path, fat_ctx_t** out) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    if (path == NULL)
        return FAT_ERR_INVALID_ARG;

    FILE* fp = fopen(path, "rb");
    if (fp == NULL)
        return FAT_ERR_IO;

    // the whole image is loaded; find out how large it is
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return FAT_ERR_IO;
    }
    long fileSize = ftell(fp);
    if (fileSize < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return FAT_ERR_IO;
    }

    uint8_t* image = malloc((size_t)fileSize);
    if (image == NULL) {
        fclose(fp);
        return FAT_ERR_NOMEM;
    }
    if (fread(image, 1, (size_t)fileSize, fp) != (size_t)fileSize) {
        free(image);
        fclose(fp);
        return FAT_ERR_IO;
    }
    fclose(fp);

    // validate + copy into a context (fat_ctx_init_mem copies again; the
    // copy here keeps the FILE* read out of the core)
    fat_result_t r = fat_ctx_init_mem(out, image, (size_t)fileSize);
    free(image);
    return r;
}
