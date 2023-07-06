#include "color.h"
#include "fat.h"
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// function declaration
void check_null(void* p);
void dump_entry(DirectoryEntry* e);

// functions
void check_null(void* p) {
    if (p == NULL) {
        fprintf(stderr, "p is NULL.\n");
        exit(1);
    }
}

int main() {
    char* fat_path = "demof12.fat";
    FILE* fp = fopen(fat_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open the fat image '%s'.\n", fat_path);
        return 1;
    }

    // ref: https://free.pjc.co.jp/fat/mem/fatm122.html
    // FAT12
    bool ret = fat_init(fp);
    if (!ret) {
        fprintf(stderr, "fat_init failed\n");
        return 1;
    }
    fclose(fp);

    printf("*** FAT info ***\n");
    fat_print_info();
    printf("*** BIOS parameter block ***\n");
    fat_print_header_legend();
    fat_print_header_dump();

    /* printf("*** FAT table ***\n"); */
    /* fat_print_fat12(); */

    printf("*** Files and Directories ***\n");
    fat_print_directory_entry_header_legend();
    iterate_dir(0);

    return 0;
}
