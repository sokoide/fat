# FAT12/16/32 lib

A small C library (C99, no dependencies) for reading and writing FAT
filesystem images, plus a demo CLI that dumps them.

## Features

* FAT12, FAT16 and FAT32 images (FAT32 root cluster chains and FSInfo included)
* Read: directory iteration, path lookup (`a/b/c.txt`, `.`/`..`), whole-file
  and streaming (`fat_file_t` cursor) reads
* Write: create/overwrite/truncate/unlink/rmdir, cluster alloc/free,
  LFN entries with generated `BASE~N` aliases
* Long file names: LFN runs are joined on read (checksum-validated, UTF-8
  rendered) and written for names beyond 8.3
* 8.3 names render with the NTRes lowercase flags (`hello.txt`, not
  `HELLO.TXT`) when the on-disk entry carries them
* Pluggable I/O backends (RAM / stdio file / your own `fat_io_t`), with a
  write-back sector cache -- mount images, block devices, FUSE, ...

## Prerequisites

* clang (or any C99 compiler) and make
* mtools (fixture generation)

```sh
# macOS
brew install mtools
# Linux
sudo apt install mtools
```

## How to build & run the demo

```sh
make            # build demo (build/fatdemo)
make run        # run it against the committed FAT12 fixture (demof12.fat)
```

`make fat16` / `make fat32` generate bigger local fixtures (gitignored);
`make demo12` / `demo16` / `demo32` run the demo per image type, and
`make demo-all` regenerates everything and runs all three.

The demo prints the BPB, FAT table, directory entry dumps (with raw
bytes), `ls`-style views, multi-cluster file reads, and a write-API
section that creates/appends/truncates/unlinks a file on a private
in-memory copy -- the on-disk image is never touched.

## How to run the tests

```sh
make check      # normal + ASan/UBSan suites (FAT12 fixture is committed)
make check-all  # regenerate all three fixtures with mtools, then check
```

## Repository layout

| file          | role                                                    |
| ------------- | ------------------------------------------------------- |
| fat.h         | public API (everything below lives here)                 |
| fat_core.c    | core: open, iterate, lookup, read/write, LFN, FAT ops    |
| fat_dev.c     | I/O backends + sector cache                              |
| fat_dump.c    | pretty printers (BPB / FAT / directory entries)          |
| color.c       | terminal coloring for the dump views                     |
| main.c        | demo CLI                                                 |
| tests/        | test suite: one file per area (read / write / io / lfn / |
|               | mtools oracle), a tiny EXPECT framework in test_util.h   |
|               | with per-test failure isolation, plus the shared helpers |

## More info

* The complete API reference is the comment block per function in
  [fat.h](./fat.h) (lifecycle, introspection, iteration, lookup,
  streaming access, timestamps, FSInfo, write support, I/O backends).
* [REVIEW.md](./REVIEW.md) records the design and verification log of
  every development phase.
