// fat_dev.c -- ship I/O backends (RAM private copy, stdio file) and the
// thin open/export wrappers on top of fat_open_io; the only stdio user on
// the library side. Never prints: failures propagate as fat_result_t.

#include "fat_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the boot sector every context reads at open; a shorter image cannot
// hold a BPB at all
#define DEV_BOOT_SECTOR 512u

// RAM backend (fat_io_mem) ---------------------------------------------------

typedef struct mem_io {
    fat_io_t io;    // must stay first: (mem_io_t*)io recovers this struct
    uint8_t* copy;  // private copy; the caller's buffer is never touched
    size_t size;
} mem_io_t;

static fat_result_t mem_io_read(fat_io_t* io, uint64_t offset, void* buf,
                                size_t len) {
    mem_io_t* m = (mem_io_t*)io;
    if (len == 0)
        return FAT_OK;
    if (offset > m->size || len > m->size - (size_t)offset)
        return FAT_ERR_IO;
    memcpy(buf, m->copy + (size_t)offset, len);
    return FAT_OK;
}

static fat_result_t mem_io_write(fat_io_t* io, uint64_t offset,
                                 const void* buf, size_t len) {
    mem_io_t* m = (mem_io_t*)io;
    if (len == 0)
        return FAT_OK;
    if (offset > m->size || len > m->size - (size_t)offset)
        return FAT_ERR_IO;
    memcpy(m->copy + (size_t)offset, buf, len);
    return FAT_OK;
}

static uint64_t mem_io_size(const fat_io_t* io) {
    return ((const mem_io_t*)io)->size;
}

static void mem_io_close(fat_io_t* io) {
    mem_io_t* m = (mem_io_t*)io;
    free(m->copy);
    free(m);
}

fat_result_t fat_io_mem(fat_io_t** out, const uint8_t* image, size_t size) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    if (image == NULL && size > 0)
        return FAT_ERR_INVALID_ARG;

    mem_io_t* m = malloc(sizeof(*m));
    if (m == NULL)
        return FAT_ERR_NOMEM;
    m->copy = malloc(size); // malloc(0) may return NULL: a zero-size backend
    if (m->copy == NULL && size > 0) {
        free(m);
        return FAT_ERR_NOMEM;
    }
    if (size > 0)
        memcpy(m->copy, image, size);
    m->size = size;
    m->io.read = mem_io_read;
    m->io.write = mem_io_write;
    m->io.size = mem_io_size;
    m->io.close = mem_io_close;
    *out = &m->io;
    return FAT_OK;
}

// stdio file backend (fat_io_file) --------------------------------------------

typedef struct file_io {
    fat_io_t io;    // must stay first
    FILE* f;
    uint64_t size;  // cached at construction: backends are fixed-size
    bool writable;  // opened "r+b"; the read-only fallback rejects writes
} file_io_t;

static fat_result_t file_io_read(fat_io_t* io, uint64_t offset, void* buf,
                                 size_t len) {
    file_io_t* fi = (file_io_t*)io;
    if (len == 0)
        return FAT_OK;
    if (offset > fi->size || (uint64_t)len > fi->size - offset)
        return FAT_ERR_IO;
    if (fseek(fi->f, (long)offset, SEEK_SET) != 0)
        return FAT_ERR_IO;
    if (fread(buf, 1, len, fi->f) != len)
        return FAT_ERR_IO; // short read: the file shrank under us
    return FAT_OK;
}

static fat_result_t file_io_write(fat_io_t* io, uint64_t offset,
                                  const void* buf, size_t len) {
    file_io_t* fi = (file_io_t*)io;
    if (len == 0)
        return FAT_OK;
    if (!fi->writable)
        return FAT_ERR_IO;
    if (offset > fi->size || (uint64_t)len > fi->size - offset)
        return FAT_ERR_IO;
    if (fseek(fi->f, (long)offset, SEEK_SET) != 0)
        return FAT_ERR_IO;
    if (fwrite(buf, 1, len, fi->f) != len)
        return FAT_ERR_IO; // short write: the destination refused bytes
    return FAT_OK;
}

static uint64_t file_io_size(const fat_io_t* io) {
    return ((const file_io_t*)io)->size;
}

static void file_io_close(fat_io_t* io) {
    file_io_t* fi = (file_io_t*)io;
    fclose(fi->f);
    free(fi);
}

fat_result_t fat_io_file(fat_io_t** out, const char* path) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    if (path == NULL)
        return FAT_ERR_INVALID_ARG;

    FILE* f = fopen(path, "r+b");
    bool writable = true;
    if (f == NULL) {
        // fall back to read-only (permissions, spuriously busy); writes
        // then fail with FAT_ERR_IO when dirty sectors are flushed
        f = fopen(path, "rb");
        writable = false;
    }
    if (f == NULL)
        return FAT_ERR_IO;

    // the extent is fixed for the backend's lifetime; measure it once
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FAT_ERR_IO;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        return FAT_ERR_IO;
    }

    file_io_t* fi = malloc(sizeof(*fi));
    if (fi == NULL) {
        fclose(f);
        return FAT_ERR_NOMEM;
    }
    fi->f = f; // every transfer seeks, so the FILE* may stay at the end
    fi->size = (uint64_t)end;
    fi->writable = writable;
    fi->io.read = file_io_read;
    fi->io.write = file_io_write;
    fi->io.size = file_io_size;
    fi->io.close = file_io_close;
    *out = &fi->io;
    return FAT_OK;
}

// open wrappers ----------------------------------------------------------------

// Hand a freshly built backend to fat_open_io and honour the ownership
// contract: on failure the caller keeps the backend, so the wrapper closes
// what it constructed itself.
static fat_result_t open_via(fat_io_t* io, fat_ctx_t** out) {
    fat_result_t r = fat_open_io(io, out);
    if (r != FAT_OK && io->close != NULL)
        io->close(io);
    return r;
}

fat_result_t fat_open(const char* path, fat_ctx_t** out) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    if (path == NULL)
        return FAT_ERR_INVALID_ARG;

    fat_io_t* io = NULL;
    fat_result_t r = fat_io_file(&io, path);
    if (r != FAT_OK)
        return r;
    return open_via(io, out);
}

fat_result_t fat_open_mem(const uint8_t* image, size_t size, fat_ctx_t** out) {
    if (out == NULL)
        return FAT_ERR_INVALID_ARG;
    *out = NULL;
    if (image == NULL)
        return FAT_ERR_INVALID_ARG;
    // kept from the whole-image era: an image shorter than the boot sector
    // is a malformed BPB, not an I/O failure (fat_open_io alone would
    // report FAT_ERR_IO for any backend too small to serve the boot read)
    if (size < DEV_BOOT_SECTOR)
        return FAT_ERR_INVALID_BPB;

    fat_io_t* io = NULL;
    fat_result_t r = fat_io_mem(&io, image, size);
    if (r != FAT_OK)
        return r;
    return open_via(io, out);
}

// whole-image export -------------------------------------------------------------

fat_result_t fat_write(const fat_ctx_t* ctx, const char* path) {
    if (ctx == NULL || path == NULL)
        return FAT_ERR_INVALID_ARG;

    FILE* fp = fopen(path, "wb");
    if (fp == NULL)
        return FAT_ERR_IO;

    // Reading only fills the sector cache; the logical image is unchanged,
    // so dropping const is semantically safe here.
    fat_ctx_t* c = (fat_ctx_t*)ctx;

    // [0, image_size) in chunks, through the cache: sectors whose writes
    // have not been flushed to the backend yet are exported too
    uint8_t chunk[4096];
    uint64_t total = c->image_size;
    for (uint64_t off = 0; off < total; off += sizeof(chunk)) {
        size_t len = total - off < sizeof(chunk) ? (size_t)(total - off)
                                                 : sizeof(chunk);
        fat_result_t r = fat_io_read(c, off, chunk, len);
        if (r != FAT_OK) {
            fclose(fp);
            return r;
        }
        if (fwrite(chunk, 1, len, fp) != len) {
            fclose(fp);
            return FAT_ERR_IO;
        }
    }
    if (fclose(fp) != 0)
        return FAT_ERR_IO;
    return FAT_OK;
}
