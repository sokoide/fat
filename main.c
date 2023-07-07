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
void cat_file(uint32_t current_cluster, const char* path);
void cat_file_for_cluster(uint32_t cluster, uint32_t file_size);

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

void cat_file(uint32_t current_cluster, const char* path) {
    DirectoryEntry entry;
    char* token;
    const char* delim = "/";
    char tmp_path[64];
    strcpy(tmp_path, path);

    token = strtok(tmp_path, delim);
    uint32_t prev_cluster;
    uint32_t cluster = current_cluster;
    while (token) {
        prev_cluster = cluster;
        fat_set_entry_name(&entry, token);
        cluster = fat_get_cluster_for_entry(prev_cluster, &entry);
        token = strtok(NULL, delim);
    }
    cat_file_for_cluster(cluster, entry.fileSize);
}

void cat_file_for_cluster(uint32_t cluster, uint32_t file_size) {
    char buffer[1025];
    while (file_size > 0) {
        uint8_t* p = fat_get_cluster_ptr(cluster);
        if (file_size >= 1024) {
            memcpy(buffer, p, 1024);
            buffer[1024] = '\0';
            file_size -= 1024;
            cluster = fat_get_fat(cluster);
        } else {
            memcpy(buffer, p, file_size);
            buffer[file_size] = '\0';
            file_size = 0;
        }
        printf("%s", buffer);
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
    uint32_t cluster = fat_get_cluster_for_entry(0, &entry);
    printf("cluster: %u\n", cluster);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** ls /dir2 ***\n");
    fat_set_entry_name(&entry, "dir2");
    cluster = fat_get_cluster_for_entry(0, &entry);
    printf("cluster: %u\n", cluster);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** ls /dir2/subbdir1 ***\n");
    fat_set_entry_name(&entry, "subdir1");
    cluster = fat_get_cluster_for_entry(cluster, &entry);
    printf("cluster: %u\n", cluster);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** cat /dir1/hoge.txt *** \n");
    cat_file(0, "dir1/hoge.txt");

    printf("*** cat /test_5kb.txt *** \n");
    cat_file(0, "test_5kb.txt");

    printf("*** cat /dir2/subdir1/page.txt *** \n");
    cat_file(0, "dir2/subdir1/page.txt");

    return 0;
}
