#include "fat.h"
#include <stdbool.h>
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
static const char* type_name(const fat_ctx_t* ctx);
static void callback_ls(const fat_dirent_t* entry, const uint8_t* raw32,
                        void* user);
static void iterate_directory(fat_ctx_t* ctx, uint32_t dir_cluster,
                              fat_iter_cb cb);
static fat_result_t lookup_quiet(fat_ctx_t* ctx, const char* path,
                                 fat_dirent_t* out);
static void dump_section(fat_ctx_t* ctx, const char* path);
static void ls_section(fat_ctx_t* ctx, const char* path);
static void cat_file(fat_ctx_t* ctx, const char* path);

// functions
static const char* type_name(const fat_ctx_t* ctx) {
    switch (fat_get_type(ctx)) {
    case FT_FAT12:
        return "FAT12";
    case FT_FAT16:
        return "FAT16";
    case FT_FAT32:
        return "FAT32";
    default:
        return "FAT?";
    }
}

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

// iterate one directory (FAT_CLUSTER_ROOT for the root), reporting errors
static void iterate_directory(fat_ctx_t* ctx, uint32_t dir_cluster,
                              fat_iter_cb cb) {
    fat_result_t ret = fat_iter_dir(ctx, dir_cluster, cb, NULL);
    if (ret != FAT_OK) {
        fprintf(stderr, "iterate directory: %s\n", fat_strerror(ret));
    }
}

// resolve `path` from the root without any diagnostic output; the demo
// shows a section only when the path exists in this particular fixture
static fat_result_t lookup_quiet(fat_ctx_t* ctx, const char* path,
                                 fat_dirent_t* out) {
    fat_result_t ret = fat_lookup(ctx, FAT_CLUSTER_ROOT, path, out);
    if (ret == FAT_OK && !(out->attributes & 0x10))
        return FAT_ERR_PATH_NOT_FOUND; // ls/dump targets are directories
    return ret;
}

// one "* <path>" entry-dump section (fat_lookup has no "/" form, so the
// root section is dumped directly by the caller)
static void dump_section(fat_ctx_t* ctx, const char* path) {
    printf("* %s\n", path);
    fat_dirent_t entry;
    if (lookup_quiet(ctx, path, &entry) == FAT_OK) {
        iterate_directory(ctx, entry.first_cluster,
                          fat_print_directory_entry_dump);
    } else {
        printf("(not present in this image)\n");
    }
}

// one "*** ls <path> ***" section, shown only when the path exists
static void ls_section(fat_ctx_t* ctx, const char* path) {
    fat_dirent_t entry;
    if (lookup_quiet(ctx, path, &entry) != FAT_OK)
        return; // absent in this fixture type: skip the section entirely
    printf("*** %s: ls /%s ***\n", type_name(ctx), path);
    printf("cluster: %u\n", entry.first_cluster);
    iterate_directory(ctx, entry.first_cluster, callback_ls);
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

// FAT32 only: the root directory lives in a cluster chain, not a fixed
// region -- walk it cluster by cluster through the FAT
static void root_chain_section(fat_ctx_t* ctx) {
    const fat_geometry_t* geo = fat_geometry(ctx);
    printf("*** FAT32: root directory cluster chain ***\n");
    printf("root_cluster: %u\n", geo->root_cluster);
    uint32_t c = geo->root_cluster;
    for (uint32_t visited = 0; c >= 2 && visited <= geo->cluster_count + 2;
         visited++) {
        uint32_t next = FAT_CLUSTER_NOT_FOUND;
        if (fat_get_fat_entry(ctx, c, &next) != FAT_OK) {
            printf("cluster %u: FAT read failed\n", c);
            return;
        }
        printf("cluster %u -> %u (0x%X)\n", c, next, next);
        if (next >= 0x0FFFFFF8u) // FAT32 end-of-chain (masked value)
            return;
        if (next < 2) { // free or reserved: broken chain, stop walking
            printf("chain broken at %u\n", next);
            return;
        }
        c = next;
    }
    printf("chain longer than the image: giving up\n");
}

int main(int argc, char** argv) {
    // ./fatdemo [image-path]: no argument keeps the FAT12 fixture demo
    const char* fat_path = argc > 1 ? argv[1] : "demof12.fat";
    fat_ctx_t* ctx = NULL;

    fat_result_t ret = fat_open(fat_path, &ctx);
    if (ret != FAT_OK) {
        fprintf(stderr, "fat_open '%s': %s\n", fat_path, fat_strerror(ret));
        fat_close(ctx); // no-op on NULL
        return 1;
    }
    const bool is_fat12 = fat_get_type(ctx) == FT_FAT12;
    const bool is_fat32 = fat_get_type(ctx) == FT_FAT32;

    // ---- sections shared by every image type ----
    printf("*** FAT info (%s, %s) ***\n", type_name(ctx), fat_path);
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
    dump_section(ctx, "dir1");
    if (is_fat12) {
        // FAT12 fixture: dir1/subdir1, dir2 and its subdir1
        dump_section(ctx, "dir1/subdir1");
        dump_section(ctx, "dir2");
        dump_section(ctx, "dir2/subdir1");
    } else {
        // FAT16/FAT32 fixtures: dir1/sub1
        dump_section(ctx, "dir1/sub1");
    }

    printf("*** ls / ***\n");
    iterate_directory(ctx, FAT_CLUSTER_ROOT, callback_ls);
    ls_section(ctx, "dir1");
    if (is_fat12) {
        ls_section(ctx, "dir1/subdir1");
        ls_section(ctx, "dir2");
        ls_section(ctx, "dir2/subdir1");
    } else {
        ls_section(ctx, "dir1/sub1");
    }

    if (is_fat32) {
        // ---- FAT32-only sections ----
        fat_fsinfo_t fi;
        printf("*** FAT32: FSInfo ***\n");
        if (fat_fsinfo(ctx, &fi) == FAT_OK) {
            printf("free_cluster_count: %u\n", fi.free_cluster_count);
            printf("next_free_cluster: %u\n", fi.next_free_cluster);
        } else {
            printf("fsinfo: unavailable\n");
        }
        root_chain_section(ctx);
    }

    printf("*** cat /hello.txt ***\n");
    cat_file(ctx, "hello.txt");
    printf("*** cat /test_5kb.txt ***\n");
    cat_file(ctx, "test_5kb.txt");
    printf("*** cat /dir1/hoge.txt ***\n");
    cat_file(ctx, "dir1/hoge.txt");
    if (is_fat12) {
        printf("*** cat /dir2/subdir1/page.txt ***\n");
        cat_file(ctx, "dir2/subdir1/page.txt");
    } else {
        printf("*** cat /dir1/sub1/page.txt ***\n");
        cat_file(ctx, "dir1/sub1/page.txt");
    }

    // '.' stays in the current directory, '..' walks back to the root
    printf("*** cat /dir1/../hello.txt ***\n");
    cat_file(ctx, "dir1/../hello.txt");

    fat_close(ctx);
    return 0;
}
