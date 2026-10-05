#include "color.h"
#include "fat.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// function declaration
void callback_ls(DirectoryEntry* entry, void* p);
uint32_t cluster_for_path(uint32_t current_cluster, const char* path,
                          DirectoryEntry* entry);
void cat_file(uint32_t current_cluster, const char* path);
void cat_file_for_cluster(uint32_t cluster, uint32_t file_size);

// functions
void callback_ls(DirectoryEntry* entry, void* p) {
    (void)p;
    char name[13]; // 8 + '.' + 3 + '\0'
    if (fat_get_entry_name(entry, name, sizeof(name) / sizeof(name[0])) ==
        NULL) {
        return;
    }
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

uint32_t cluster_for_path(uint32_t current_cluster, const char* path,
                          DirectoryEntry* entry) {
    char* token;
    char* next;
    char* saveptr;
    const char* delim = "/";
    char tmp_path[64];

    if (snprintf(tmp_path, sizeof(tmp_path), "%s", path) >=
        (int)sizeof(tmp_path)) {
        fprintf(stderr, "path too long: '%s'.\n", path);
        return FAT_CLUSTER_NOT_FOUND;
    }

    uint32_t cluster = current_cluster;
    token = strtok_r(tmp_path, delim, &saveptr);
    while (token) {
        if (!fat_set_entry_name(entry, token)) {
            // not representable as an 8.3 name
            fprintf(stderr, "invalid name: '%s'.\n", token);
            return FAT_CLUSTER_NOT_FOUND;
        }
        cluster = fat_get_cluster_for_entry(cluster, entry);
        if (cluster == FAT_CLUSTER_NOT_FOUND) {
            return FAT_CLUSTER_NOT_FOUND;
        }
        next = strtok_r(NULL, delim, &saveptr);
        if (next != NULL && !(entry->attributes & 0x10)) {
            // intermediate path components must be directories
            fprintf(stderr, "'%s' is not a directory.\n", token);
            return FAT_CLUSTER_NOT_FOUND;
        }
        token = next;
    }
    return cluster;
}

void cat_file(uint32_t current_cluster, const char* path) {
    DirectoryEntry entry;
    uint32_t cluster = cluster_for_path(current_cluster, path, &entry);
    if (cluster != FAT_CLUSTER_NOT_FOUND) {
        cat_file_for_cluster(cluster, entry.fileSize);
    } else {
        fprintf(stderr, "path not found.\n");
    }
}

void cat_file_for_cluster(uint32_t cluster, uint32_t file_size) {
    uint32_t cluster_size = fat_get_cluster_size();
    if (cluster_size == 0) {
        fprintf(stderr, "invalid cluster size.\n");
        return;
    }
    while (file_size > 0) {
        uint8_t* p = fat_get_cluster_ptr(cluster);
        if (p == NULL) {
            fprintf(stderr, "broken cluster chain at cluster %u.\n", cluster);
            return;
        }
        uint32_t bytes_this =
            (file_size < cluster_size) ? file_size : cluster_size;
        fwrite(p, 1, bytes_this, stdout);
        file_size -= bytes_this;
        if (file_size > 0) {
            // data remains: the current cluster must have a successor
            if (fat_is_end_of_cluster(cluster) || fat_is_broken(cluster)) {
                fprintf(stderr,
                        "warning: cluster chain ended at %u with %u bytes "
                        "remaining.\n",
                        cluster, file_size);
                return;
            }
            cluster = fat_get_fat(cluster);
        }
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
    uint32_t cluster = cluster_for_path(0, "dir1", &entry);
    printf("cluster: %u\n", cluster);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** ls /dir2 ***\n");
    cluster = cluster_for_path(0, "dir2", &entry);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** ls /dir2/subdir1 ***\n");
    cluster = cluster_for_path(0, "/dir2/subdir1", &entry);
    iterate_dir(cluster, callback_ls, NULL);

    printf("*** cat /dir1/hoge.txt *** \n");
    cat_file(0, "dir1/hoge.txt");

    printf("*** cat /test_5kb.txt *** \n");
    cat_file(0, "test_5kb.txt");

    printf("*** cat /dir2/subdir1/page.txt *** \n");
    cat_file(0, "dir2/subdir1/page.txt");

    fat_uninit();
    return 0;
}
