#include "color.h"
#include "fat.h"
#include <memory.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// function declaration
void check_null(void* p);
void callback_ls(DirectoryEntry* entry, void* p);

// functions
void check_null(void* p) {
    if (p == NULL) {
        fprintf(stderr, "p is NULL.\n");
        exit(1);
    }
}

void callback_ls(DirectoryEntry* entry, void* p) {
    char name[12];
    fat_get_entry_name(entry, name, sizeof(name) / sizeof(name[0]));
    if (entry->attributes & 0x10) {
        // Directory
        printf("D %s\n", name);
    } else if (entry->attributes & 0x08) {
        // Volume Label
        printf("V %s\n", name);
    } else {
        // File
        printf("F %s %u\n", name, entry->fileSize);
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

    printf("*** FAT table ***\n");
    fat_print_fat12();

    printf("*** Files and Directories ***\n");
    fat_print_directory_entry_header_legend();
    printf("* /\n");
    iterate_dir(0, fat_print_directory_entry_dump, NULL);
    printf("* /dir1\n");
    iterate_dir(8, fat_print_directory_entry_dump, NULL);
    printf("* /dir2\n");
    iterate_dir(11, fat_print_directory_entry_dump, NULL);

    printf("*** ls / ***\n");
    iterate_dir(0, callback_ls, NULL);

    printf("*** ls /dir1 ***\n");
    DirectoryEntry entry;
    fat_set_entry_name(&entry, "dir1");
    uint32_t cluster_dir1 = fat_get_cluster_for_entry(0, &entry);
    printf("cluster: %u\n", cluster_dir1);
    iterate_dir(cluster_dir1, callback_ls, NULL);

    printf("*** ls /dir2 ***\n");
    fat_set_entry_name(&entry, "dir2");
    uint32_t cluster_dir2 = fat_get_cluster_for_entry(0, &entry);
    printf("cluster: %u\n", cluster_dir2);
    iterate_dir(cluster_dir2, callback_ls, NULL);

    printf("*** ls /dir2/subbdir1 ***\n");
    fat_set_entry_name(&entry, "subdir1");
    uint32_t cluster = fat_get_cluster_for_entry(cluster_dir2, &entry);
    printf("cluster: %u\n", cluster);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** cat /dir1/hoge.txt *** \n");
    fat_set_entry_name(&entry, "hoge.txt");
    cluster = fat_get_cluster_for_entry(cluster_dir1, &entry);
    printf("cluster: %u, size: %u\n", cluster, entry.fileSize);
    char* p = fat_get_cluster_ptr(cluster);
    // TODO: handle multi lines, >1024 byte file
    printf("%s\n", p);

    printf("*** cat /test_5kb.txt *** \n");
    fat_set_entry_name(&entry, "test_5kb.txt");
    cluster = fat_get_cluster_for_entry(0, &entry);
    printf("cluster: %u, size: %u\n", cluster, entry.fileSize);
    p = fat_get_cluster_ptr(cluster);
    // TODO: handle multi lines, >1024 byte file
    printf("%s\n", p);

    return 0;
}
