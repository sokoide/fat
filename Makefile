TARGET = fatdemo
TESTTARGET = fattest
OUTDIR = ~/tmp/hoge

COMMONFILES =
SRCS = main.c \
	   fat.c \
	   color.c
TESTSRCS = testmain.c \
		   fat.c \
		   color.c

HEADERS :=  $(shell find . -type f -name '*.h')
OBJS = $(SRCS:.c=.o)
TESTOBJS = $(TESTSRCS:.c=.o)

OUTOBJS = $(addprefix $(OUTDIR)/,$(OBJS))
TESTOUTOBJS = $(addprefix $(OUTDIR)/,$(TESTOBJS))

CXX = clang
CXXFLAGS = -std=c99 -Wall -g -I.

VOL := DEMOF12
IMG := demof12.fat

.PHONY: default build testbuild run test diag fat12 clean

default: $(TARGET)

$(TARGET): build
$(TESTTARGET): testbuild

build: $(OUTOBJS)
	$(CXX) $(CXXFLAGS) $^ -o $(OUTDIR)/$(TARGET) $(LDFLAGS)

testbuild: $(TESTOUTOBJS)
	$(CXX) $(CXXFLAGS) $^ -o $(OUTDIR)/$(TESTTARGET) $(LDFLAGS)

$(OUTDIR)/%.o : %.c | $(OUTDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUTDIR):
	mkdir -p $(OUTDIR)

run: $(TARGET)
	$(OUTDIR)/$(TARGET)

test: $(TESTTARGET)
	$(OUTDIR)/$(TESTTARGET)

diag: $(TARGET)
	#readelf -d $(OUTDIR)/$(TARGET)
	objdump -p $(OUTDIR)/$(TARGET)

fat12:
	rm $(IMG)
	mformat -f 720 -v $(VOL) -C -i $(IMG) ::

	echo "hello world" > hello.txt
	echo "hello world2" > hello2.txt
	mcopy -i $(IMG) hello.txt test_5kb.txt ::

	mmd -i $(IMG) dir1 dir1/subdir1 dir1/subdir2 dir2 dir2/subdir1 dir2/subdir2 dir2/subdir3 dir2/subdir4 dir2/subdir5 dir2/subdir6 dir2/subdir7 dir2/subdir8 dir2/subdir9 dir2/subdir10 dir2/subdir11 dir2/subdir12 dir2/subdir13 dir2/subdir14 dir2/subdir15 dir2/subdir16 dir2/subdir17 dir2/subdir18 dir2/subdir19 dir2/subdir20 dir2/subdir21 dir2/subdir22 dir2/subdir23 dir2/subdir24 dir2/subdir25 dir2/subdir26 dir2/subdir27 dir2/subdir28 dir2/subdir29 dir2/subdir30 dir2/subdir31 dir2/subdir32 dir2/subdir33
	mdir -i $(IMG)
	mdir -i $(IMG) ::dir1

	echo "hoge" > hoge.txt
	echo "page" > page.txt
	mcopy -i $(IMG) hoge.txt ::dir1
	mcopy -i $(IMG) page.txt ::dir2/subdir1
	mcopy -i $(IMG) test_5kb.txt ::dir2/subdir1

	mdir -i $(IMG)
	mdir -i $(IMG) ::dir1
	mdir -i $(IMG) ::dir2
	mdir -i $(IMG) ::dir2/subdir1

	rm hello*.txt hoge.txt page.txt


clean:
	rm -rf $(OUTDIR)
