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
TESTSRCS = testmain.c \
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
CFLAGS = -std=c99 -Wall -Wextra -Wshadow -g -I. -MMD -MP
SANFLAGS = $(CFLAGS) -fsanitize=address,undefined

VOL := DEMOF12
IMG := demof12.fat

.PHONY: default testbuild run test test-san check diag fat12 clean

default: $(OUTDIR)/$(TARGET)

testbuild: $(OUTDIR)/$(TESTTARGET)

$(OUTDIR)/$(TARGET): $(OUTOBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OUTDIR)/$(TESTTARGET): $(TESTOUTOBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(SANOUTDIR)/$(TESTTARGET): $(SANOUTOBJS) | $(SANOUTDIR)
	$(CC) $(SANFLAGS) $^ -o $@ $(LDFLAGS)

$(OUTDIR)/%.o : %.c | $(OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(SANOUTDIR)/%.o : %.c | $(SANOUTDIR)
	$(CC) $(SANFLAGS) -c $< -o $@

$(OUTDIR) $(SANOUTDIR):
	mkdir -p $@

run: $(OUTDIR)/$(TARGET)
	$(OUTDIR)/$(TARGET)

test: $(OUTDIR)/$(TESTTARGET)
	$(OUTDIR)/$(TESTTARGET)

test-san: $(SANOUTDIR)/$(TESTTARGET)
	$(SANOUTDIR)/$(TESTTARGET)

check: test test-san

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

clean:
	rm -rf $(OUTDIR) $(SANOUTDIR) $(SCRATCH)

-include $(OUTOBJS:.o=.d) $(TESTOUTOBJS:.o=.d) $(SANOUTOBJS:.o=.d)
