CXX ?= g++
TARGET_GCC ?= gcc
GCCPLUGINS_DIR = $(shell $(TARGET_GCC) -print-file-name=plugin)

BUILDDIR = $(CURDIR)/build
SOURCES = $(wildcard src/*.c)
DESTDIR ?= $(BUILDDIR)/installed/

CPPFLAGS += -I$(GCCPLUGINS_DIR)/include

ifeq ($(findstring 1,$(NDEBUG)),1)
	CFLAGS += -O3 -flto
else
	CFLAGS += -O0 -g
endif

.PHONY: install_dirs build_dirs all

all: $(BUILDDIR)/extract_offsets.so

$(BUILDDIR)/extract_offsets.so: $(SOURCES) | build_dirs
	@$(CXX) -shared -fpic $(CPPFLAGS) $(CFLAGS) $^ -o $@

install: $(BUILDDIR)/extract_offsets.so | install_dirs
	@cp $< $(DESTDIR)/extract_offsets.so

clean:
	@rm -rf $(BUILDDIR)

install_dirs:
	@mkdir -p $(DESTDIR)

build_dirs:
	@mkdir -p $(BUILDDIR)
