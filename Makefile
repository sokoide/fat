TARGET = fatdemo
TESTTARGET = fattest
OUTDIR ?= build
SANOUTDIR ?= build-san
SCRATCH := scratch

SRCS = main.c \
	   fat_core.c \
	   fat_dev.c \
	   fat_dump.c \
	   color.c
TESTSRCS = tests/test_main.c \
		   tests/test_util.c \
		   tests/test_read.c \
		   tests/test_io.c \
		   tests/test_write.c \
		   tests/test_lfn.c \
		   tests/test_oracle.c \
		   fat_core.c \
		   fat_dev.c \
		   fat_dump.c \
		   color.c

OBJS = $(SRCS:.c=.o)
TESTOBJS = $(TESTSRCS:.c=.o)

OUTOBJS = $(addprefix $(OUTDIR)/,$(OBJS))
TESTOUTOBJS = $(addprefix $(OUTDIR)/,$(TESTOBJS))
SANOUTOBJS = $(addprefix $(SANOUTDIR)/,$(TESTOBJS))

CC = clang
# _POSIX_C_SOURCE: the mtools-oracle helpers use popen/pclose, which
# -std=c99 hides on glibc (macOS libc exposes them unconditionally)
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wshadow -Wstrict-prototypes -g -I. -MMD -MP
SANFLAGS = $(CFLAGS) -fsanitize=address,undefined

VOL := DEMOF12
IMG := demof12.fat
VOL16 := DEMOF16
IMG16 := demof16.fat
VOL32 := DEMOF32
IMG32 := demof32.fat

.PHONY: default testbuild run demo12 demo16 demo32 demo-all test test-san check check-all diag fat12 fat16 fat32 clean

default: $(OUTDIR)/$(TARGET)

testbuild: $(OUTDIR)/$(TESTTARGET)

$(OUTDIR)/$(TARGET): $(OUTOBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OUTDIR)/$(TESTTARGET): $(TESTOUTOBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(SANOUTDIR)/$(TESTTARGET): $(SANOUTOBJS) | $(SANOUTDIR)
	$(CC) $(SANFLAGS) $^ -o $@ $(LDFLAGS)

$(OUTDIR)/%.o : %.c | $(OUTDIR)
	@mkdir -p $(dir $@) # tests/ objects land in build/tests/
	$(CC) $(CFLAGS) -c $< -o $@

$(SANOUTDIR)/%.o : %.c | $(SANOUTDIR)
	@mkdir -p $(dir $@)
	$(CC) $(SANFLAGS) -c $< -o $@

$(OUTDIR) $(SANOUTDIR):
	mkdir -p $@

run: $(OUTDIR)/$(TARGET)
	$(OUTDIR)/$(TARGET)

# phase 8: run the demo against each fixture type.  demof12.fat is
# committed; demof16/demof32.fat are local (make fat16/fat32) -- the
# targets skip with a hint when the image is absent.
demo12: $(OUTDIR)/$(TARGET)
	$(OUTDIR)/$(TARGET) $(IMG)

demo16: $(OUTDIR)/$(TARGET)
	@if [ -f $(IMG16) ]; then $(OUTDIR)/$(TARGET) $(IMG16); \
	else echo "$(IMG16) not found -- generate it with 'make fat16'"; fi

demo32: $(OUTDIR)/$(TARGET)
	@if [ -f $(IMG32) ]; then $(OUTDIR)/$(TARGET) $(IMG32); \
	else echo "$(IMG32) not found -- generate it with 'make fat32'"; fi

demo-all: fat16 fat32 demo12 demo16 demo32

test: $(OUTDIR)/$(TESTTARGET)
	$(OUTDIR)/$(TESTTARGET)

test-san: $(SANOUTDIR)/$(TESTTARGET)
	$(SANOUTDIR)/$(TESTTARGET)

check: test test-san

# everything: regenerate all three fixtures (mtools), then run the suites.
# plain `check` skips the FAT16/FAT32 suites when those images are absent.
check-all: fat12 fat16 fat32 check

diag: $(OUTDIR)/$(TARGET)
	#readelf -d $(OUTDIR)/$(TARGET)
	objdump -p $(OUTDIR)/$(TARGET)

# Regenerate the fixture image. test_5kb.txt is a committed fixture file and
# must exist in the repo root; the other files are throwaway scratch files.
fat12:
	rm -f $(IMG)
	mkdir -p $(SCRATCH) && trap 'rm -rf $(SCRATCH)' EXIT && \
	echo "hello world" > $(SCRATCH)/hello.txt && \
	echo "hello world2" > $(SCRATCH)/hello2.txt && \
	mformat -f 720 -v $(VOL) -C -i $(IMG) :: && \
	mcopy -i $(IMG) $(SCRATCH)/hello.txt test_5kb.txt :: && \
	mmd -i $(IMG) dir1 dir1/subdir1 dir1/subdir2 dir2 dir2/subdir1 dir2/subdir2 dir2/subdir3 dir2/subdir4 dir2/subdir5 dir2/subdir6 dir2/subdir7 dir2/subdir8 dir2/subdir9 dir2/subdir10 dir2/subdir11 dir2/subdir12 dir2/subdir13 dir2/subdir14 dir2/subdir15 dir2/subdir16 dir2/subdir17 dir2/subdir18 dir2/subdir19 dir2/subdir20 dir2/subdir21 dir2/subdir22 dir2/subdir23 dir2/subdir24 dir2/subdir25 dir2/subdir26 dir2/subdir27 dir2/subdir28 dir2/subdir29 dir2/subdir30 dir2/subdir31 dir2/subdir32 dir2/subdir33 && \
	echo "I am hoge." > $(SCRATCH)/hoge.txt && \
	echo "You are page." > $(SCRATCH)/page.txt && \
	mcopy -i $(IMG) $(SCRATCH)/hoge.txt ::dir1 && \
	mcopy -i $(IMG) $(SCRATCH)/page.txt ::dir2/subdir1 && \
	mcopy -i $(IMG) test_5kb.txt ::dir2/subdir1 && \
	mdir -i $(IMG) && \
	mdir -i $(IMG) ::dir1 && \
	mdir -i $(IMG) ::dir2 && \
	mdir -i $(IMG) ::dir2/subdir1 && \
	rm -rf $(SCRATCH)

# 16 MiB image: mformat defaults give 512B sectors/clusters, FATSz16=127,
# 512 root entries -> (32768-287)/1 = 32481 data clusters, inside the FAT16
# range [4085, 65524].  Smaller defaults (<= 14 MiB) come out FAT12.
# demof16.fat is gitignored (16 MiB); regenerate with this target.
fat16:
	rm -f $(IMG16)
	mkdir -p $(SCRATCH) && trap 'rm -rf $(SCRATCH)' EXIT && \
	echo "hello world" > $(SCRATCH)/hello.txt && \
	echo "You are page." > $(SCRATCH)/page.txt && \
	echo "I am hoge." > $(SCRATCH)/hoge.txt && \
	mformat -C -T 32768 -v $(VOL16) -i $(IMG16) :: && \
	mcopy -i $(IMG16) $(SCRATCH)/hello.txt :: && \
	mcopy -i $(IMG16) test_5kb.txt :: && \
	mmd -i $(IMG16) dir1 dir1/sub1 && \
	mcopy -i $(IMG16) $(SCRATCH)/page.txt ::dir1/sub1 && \
	mcopy -i $(IMG16) $(SCRATCH)/hoge.txt ::dir1 && \
	mdir -i $(IMG16) -/ :: && \
	mdir -i $(IMG16) ::dir1 && \
	mdir -i $(IMG16) ::dir1/sub1 && \
	rm -rf $(SCRATCH)

# 33 MiB image (-F forces FAT32): 512B sectors/clusters, reserved 32,
# FATSz32=520 -> dataStart 1072, (67584-1072)/1 = 66512 >= 65525 clusters.
# 32 MiB would fall 29 clusters short of the FAT32 threshold.  The 40
# F-files push the root directory to 44 entries = 3 clusters (2->54->55), so
# the root cluster chain itself is exercised.  demof32.fat is gitignored.
fat32:
	rm -f $(IMG32)
	mkdir -p $(SCRATCH) && trap 'rm -rf $(SCRATCH)' EXIT && \
	echo "hello world" > $(SCRATCH)/hello.txt && \
	echo "You are page." > $(SCRATCH)/page.txt && \
	echo "I am hoge." > $(SCRATCH)/hoge.txt && \
	for i in $$(seq 0 39); do echo "filler file $$i" > $(SCRATCH)/f$$(printf %02d $$i).txt; done && \
	mformat -C -F -T 67584 -v $(VOL32) -i $(IMG32) :: && \
	mcopy -i $(IMG32) $(SCRATCH)/hello.txt :: && \
	mcopy -i $(IMG32) test_5kb.txt :: && \
	mcopy -i $(IMG32) $(SCRATCH)/f??.txt :: && \
	mmd -i $(IMG32) dir1 dir1/sub1 && \
	mcopy -i $(IMG32) $(SCRATCH)/page.txt ::dir1/sub1 && \
	mcopy -i $(IMG32) $(SCRATCH)/hoge.txt ::dir1 && \
	mdir -i $(IMG32) -/ :: | tail -3 && \
	mdir -i $(IMG32) ::dir1 && \
	mdir -i $(IMG32) ::dir1/sub1 && \
	rm -rf $(SCRATCH)

clean:
	rm -rf $(OUTDIR) $(SANOUTDIR) $(SCRATCH)

-include $(OUTOBJS:.o=.d) $(TESTOUTOBJS:.o=.d) $(SANOUTOBJS:.o=.d)
