#include "fat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// fat_dump.c exports (no public header yet; keep in sync with fat_dump.c)
void fat_print_info(const fat_ctx_t* ctx);
void fat_print_header_legend(void);
void fat_print_header_dump(const fat_ctx_t* ctx);
void fat_print_fat(const fat_ctx_t* ctx);
void fat_print_directory_entry_header_legend(void);
void fat_print_directory_entry_dump(const fat_dirent_t* entry,
                                    const uint8_t* raw32, void* user_data);

// function declaration
static void callback_ls(const fat_dirent_t* entry, const uint8_t* raw32,
                        void* user);
static fat_result_t lookup_dir(fat_ctx_t* ctx, const char* path,
                               fat_dirent_t* out);
static void iterate_directory(fat_ctx_t* ctx, uint32_t dir_cluster,
                              fat_iter_cb cb);
static void cat_file(fat_ctx_t* ctx, const char* path);

// functions
static void callback_ls(const fat_dirent_t* entry, const uint8_t* raw32,
                        void* user) {
    (void)raw32; // only the dump views need the raw bytes
    (void)user;
    if (entry->attributes & 0x10) {
        // Directory
        printf("D %s\n", entry->name);
    } else if (entry->attributes & 0x08) {
        // Volume Label
        printf("V %s\n", entry->name);
    } else {
        // File
        printf("F %s %u\n", entry->name, entry->file_size);
    }
}

// resolve `path` from the root and require a directory; reports errors.
// Returns FAT_OK and fills *out on success.
static fat_result_t lookup_dir(fat_ctx_t* ctx, const char* path,
                               fat_dirent_t* out) {
    fat_result_t ret = fat_lookup(ctx, FAT_CLUSTER_ROOT, path, out);
    if (ret != FAT_OK) {
        fprintf(stderr, "lookup '%s': %s\n", path, fat_strerror(ret));
        return ret;
    }
    if (!(out->attributes & 0x10)) {
        // the target of ls / a raw dump must be a directory
        fprintf(stderr, "'%s' is not a directory.\n", path);
        return FAT_ERR_PATH_NOT_FOUND;
    }
    return FAT_OK;
}

// iterate one directory (FAT_CLUSTER_ROOT for the root), reporting errors
static void iterate_directory(fat_ctx_t* ctx, uint32_t dir_cluster,
                              fat_iter_cb cb) {
    fat_result_t ret = fat_iter_dir(ctx, dir_cluster, cb, NULL);
    if (ret != FAT_OK) {
        fprintf(stderr, "iterate directory: %s\n", fat_strerror(ret));
    }
}

static void cat_file(fat_ctx_t* ctx, const char* path) {
    fat_dirent_t entry;
    fat_result_t ret = fat_lookup(ctx, FAT_CLUSTER_ROOT, path, &entry);
    if (ret != FAT_OK) {
        fprintf(stderr, "cat '%s': %s\n", path, fat_strerror(ret));
        return;
    }

    uint8_t* buf = NULL;
    size_t len = 0;
    ret = fat_read_file(ctx, &entry, &buf, &len);
    if (ret != FAT_OK) {
        fprintf(stderr, "cat '%s': %s\n", path, fat_strerror(ret));
        return;
    }
    if (len > 0) {
        fwrite(buf, 1, len, stdout);
    }
    free(buf);
}

int main(void) {
    const char* fat_path = "demof12.fat";
    fat_ctx_t* ctx = NULL;

    // ref: https://free.pjc.co.jp/fat/mem/fatm122.html
    // FAT12
    fat_result_t ret = fat_open(fat_path, &ctx);
    if (ret != FAT_OK) {
        fprintf(stderr, "fat_open '%s': %s\n", fat_path, fat_strerror(ret));
        fat_close(ctx); // no-op on NULL
        return 1;
    }

    fat_dirent_t entry;

    printf("*** FAT info ***\n");
    fat_print_info(ctx);
    printf("*** BIOS parameter block ***\n");
    fat_print_header_legend();
    fat_print_header_dump(ctx);

    printf("*** FAT table ***\n");
    fat_print_fat(ctx);

    printf("*** Files and Directories ***\n");
    fat_print_directory_entry_header_legend();
    printf("* /\n");
    iterate_directory(ctx, FAT_CLUSTER_ROOT, fat_print_directory_entry_dump);
    printf("* /dir1\n");
    if (lookup_dir(ctx, "dir1", &entry) == FAT_OK) {
        iterate_directory(ctx, entry.first_cluster,
                          fat_print_directory_entry_dump);
    }
    printf("* /dir2\n");
    if (lookup_dir(ctx, "dir2", &entry) == FAT_OK) {
        iterate_directory(ctx, entry.first_cluster,
                          fat_print_directory_entry_dump);
    }

    printf("*** ls / ***\n");
    iterate_directory(ctx, FAT_CLUSTER_ROOT, callback_ls);

    printf("*** ls /dir1 ***\n");
    if (lookup_dir(ctx, "dir1", &entry) == FAT_OK) {
        printf("cluster: %u\n", entry.first_cluster);
        iterate_directory(ctx, entry.first_cluster, callback_ls);
    }

    printf("*** ls /dir2 ***\n");
    if (lookup_dir(ctx, "dir2", &entry) == FAT_OK) {
        iterate_directory(ctx, entry.first_cluster, callback_ls);
    }

    printf("*** ls /dir2/subdir1 ***\n");
    if (lookup_dir(ctx, "dir2/subdir1", &entry) == FAT_OK) {
        iterate_directory(ctx, entry.first_cluster, callback_ls);
    }

    printf("*** cat /dir1/hoge.txt *** \n");
    cat_file(ctx, "dir1/hoge.txt");

    printf("*** cat /test_5kb.txt *** \n");
    cat_file(ctx, "test_5kb.txt");

    printf("*** cat /dir2/subdir1/page.txt *** \n");
    cat_file(ctx, "dir2/subdir1/page.txt");

    // '.' stays in the current directory, '..' walks back to the root
    printf("*** cat /dir1/../hello.txt *** \n");
    cat_file(ctx, "dir1/../hello.txt");

    fat_close(ctx);
    return 0;
}
