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

.PHONY: default build testbuild run test diag clean

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

clean:
	rm -rf $(OUTDIR)
