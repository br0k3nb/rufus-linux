# Rufus Linux port: build system
#
# Builds the port from:
#   - src/            : Linux-specific modules (this port)
#   - vendor/         : upstream vendored libraries, copied and minimally
#                       patched by tools/prepare-vendor.sh
#   - build/          : generated headers (resource tables, message IDs)
#
# Copyright © 2026 The Rufus Linux port authors
# SPDX-License-Identifier: GPL-3.0-or-later

TOPDIR     := $(abspath upstream)
BUILD      := build
VENDOR     := vendor

CC         ?= gcc
CFLAGS     ?= -O2 -g
CFLAGS     += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
              -Wno-missing-field-initializers \
              -Iinclude -I$(BUILD) -I$(TOPDIR)/src -I$(TOPDIR)/src/libcdio -I$(VENDOR)/ms-sys/inc \
              -DRUFUS_RES_ROOT=\"$(TOPDIR)\"
LDFLAGS    +=
LDLIBS     += -lpthread -lm -ldl -lcrypto

# make SANITIZE=1: build with AddressSanitizer/UBSan for debugging
ifdef SANITIZE
CFLAGS  += -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

# --- Sources ---------------------------------------------------------
PORT_SRC   := $(wildcard src/*.c)

# Upstream sources that are portable as-is (compiled in place)
PORTABLE_SRC := $(TOPDIR)/src/cregex_compile.c $(TOPDIR)/src/cregex_parse.c $(TOPDIR)/src/cregex_vm.c

MS_SYS     := br fat12 fat16 fat32 file ntfs partition_info
MS_SYS_SRC := $(addprefix $(VENDOR)/ms-sys/,$(addsuffix .c,$(MS_SYS)))

# Vendored libcdio: ISO9660 + UDF filesystem interpretation, compiled in
# place from upstream/src/libcdio with a Linux config.h (vendor/libcdio-config)
LIB_CDIO   := $(TOPDIR)/src/libcdio
CDIO_SRC   := \
	$(LIB_CDIO)/driver/disc.c $(LIB_CDIO)/driver/ds.c \
	$(LIB_CDIO)/driver/logging.c $(LIB_CDIO)/driver/memory.c \
	$(LIB_CDIO)/driver/read.c $(LIB_CDIO)/driver/sector.c \
	$(LIB_CDIO)/driver/track.c $(LIB_CDIO)/driver/util.c \
	$(LIB_CDIO)/driver/_cdio_stdio.c $(LIB_CDIO)/driver/_cdio_stream.c \
	$(LIB_CDIO)/driver/utf8.c \
	$(LIB_CDIO)/iso9660/iso9660.c $(LIB_CDIO)/iso9660/iso9660_fs.c \
	$(LIB_CDIO)/iso9660/rock.c $(LIB_CDIO)/iso9660/xa.c \
	$(LIB_CDIO)/udf/filemode.c $(LIB_CDIO)/udf/udf.c \
	$(LIB_CDIO)/udf/udf_file.c $(LIB_CDIO)/udf/udf_fs.c \
	$(LIB_CDIO)/udf/udf_time.c

# Vendored syslinux installer (libinstaller + libfat), compiled in place
# from upstream/src/syslinux - these are the exact sources Rufus uses to write
# ldlinux.sys and the Syslinux boot sector
LIB_SYSLINUX := $(TOPDIR)/src/syslinux
SYSLINUX_SRC := \
	$(LIB_SYSLINUX)/libinstaller/fs.c \
	$(LIB_SYSLINUX)/libinstaller/setadv.c \
	$(LIB_SYSLINUX)/libinstaller/syslxmod.c \
	$(LIB_SYSLINUX)/libfat/cache.c \
	$(LIB_SYSLINUX)/libfat/dumpdir.c \
	$(LIB_SYSLINUX)/libfat/fatchain.c \
	$(LIB_SYSLINUX)/libfat/open.c \
	$(LIB_SYSLINUX)/libfat/searchdir.c

# Vendored wimlib (WIM split/extract/apply), compiled in place from
# upstream/src/wimlib as a native UNIX build (no Rufus/Win32 shim), with wimlib's
# own UNIX backends from wimlib-unix/ instead of the win32_*.c ones
LIB_WIMLIB := $(TOPDIR)/src/wimlib
WIMLIB_SRC := $(filter-out $(LIB_WIMLIB)/win32_%.c,$(wildcard $(LIB_WIMLIB)/*.c))
WIMLIB_UNIX_SRC := wimlib-unix/unix_apply.c wimlib-unix/unix_capture.c

# Vendored bled (compressed disk images), from the vendor/ copy which uses
# a Linux libbb.h (see tools/prepare-vendor.sh)
BLED       := bled bled_size crc32 data_align data_extract_all data_skip decompress_bunzip2 \
              decompress_gunzip decompress_uncompress decompress_unlzma decompress_unxz decompress_unzip \
              decompress_unzstd decompress_vtsi filter_accept_all filter_accept_list filter_accept_reject_list \
              find_list_entry fse_decompress header_list header_skip header_verbose_list huf_decompress \
              init_handle open_transformer seek_by_jump seek_by_read \
              xxhash zstd_common zstd_decompress zstd_decompress_block zstd_ddict zstd_entropy_common \
              zstd_error_private
BLED_SRC   := $(addprefix $(VENDOR)/bled/,$(addsuffix .c,$(BLED)))

PORT_OBJ   := $(patsubst src/%.c,$(BUILD)/src/%.o,$(PORT_SRC))
PORTABLE_OBJ := $(patsubst $(TOPDIR)/src/%.c,$(BUILD)/portable/%.o,$(PORTABLE_SRC))
VENDOR_OBJ := $(patsubst $(VENDOR)/%.c,$(BUILD)/vendor/%.o,$(MS_SYS_SRC) $(BLED_SRC) $(VENDOR)/ezxml/xml.c)
CDIO_OBJ   := $(patsubst $(LIB_CDIO)/%.c,$(BUILD)/libcdio/%.o,$(CDIO_SRC))
SYSLINUX_OBJ := $(patsubst $(LIB_SYSLINUX)/%.c,$(BUILD)/syslinux/%.o,$(SYSLINUX_SRC))
WIMLIB_OBJ := $(patsubst $(LIB_WIMLIB)/%.c,$(BUILD)/wimlib/%.o,$(WIMLIB_SRC)) \
              $(patsubst wimlib-unix/%.c,$(BUILD)/wimlib/%.o,$(WIMLIB_UNIX_SRC))

OBJ        := $(PORT_OBJ) $(PORTABLE_OBJ) $(VENDOR_OBJ) $(CDIO_OBJ) $(SYSLINUX_OBJ) $(WIMLIB_OBJ)

GENERATED  := $(BUILD)/res_ids.h $(BUILD)/res_map.h $(BUILD)/version.h

# --- Targets ---------------------------------------------------------
.PHONY: all clean vendor gui install uninstall test

# GTK3 front-end: the shared objects without the CLI's main(), plus gui/
GUI_OBJ    := $(filter-out $(BUILD)/src/cli.o,$(OBJ)) $(BUILD)/gui/gui.o
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

gui: rufus-gui

rufus-gui: rufus $(GUI_OBJ)
	$(CC) $(GUI_OBJ) -o $@ $(LDFLAGS) $(LDLIBS) $(GTK_LIBS)

$(BUILD)/gui/%.o: gui/%.c $(GENERATED) | $(VENDOR)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Wno-deprecated-declarations -c $< -o $@

all: rufus

rufus: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

# Generated tables
$(BUILD)/res_ids.h: tools/gen_res_ids.sh $(TOPDIR)/src/resource.h
	@mkdir -p $(BUILD)
	sh tools/gen_res_ids.sh $(TOPDIR)/src $@

# Version: the upstream version this port is built from (rufus.rc FILEVERSION)
$(BUILD)/version.h: $(TOPDIR)/src/rufus.rc
	@mkdir -p $(BUILD)
	@sed -n 's/^ *FILEVERSION *\([0-9]*\),\([0-9]*\),\([0-9]*\),.*/#define RUFUS_VERSION_MAJOR \1\n#define RUFUS_VERSION_MINOR \2\n#define RUFUS_VERSION_BUILD \3/p' $< > $@
	@test -s $@ || { echo "could not get the version from rufus.rc"; rm -f $@; exit 1; }

$(BUILD)/res_map.h: tools/gen_res_map.sh $(TOPDIR)/src/rufus.rc $(TOPDIR)/src/resource.h
	@mkdir -p $(BUILD)
	sh tools/gen_res_map.sh $(TOPDIR) $@

# Vendored tree (copied minute before use)
$(VENDOR)/.stamp: tools/prepare-vendor.sh include/bled-libbb.h
	sh tools/prepare-vendor.sh $(TOPDIR)
	touch $@

# Compile rules
$(BUILD)/src/syslinux.o: CFLAGS += -I$(LIB_SYSLINUX)/libinstaller -I$(LIB_SYSLINUX)/libfat
$(BUILD)/src/wue.o $(BUILD)/src/vhd.o: CFLAGS += -I$(VENDOR)/ezxml -I$(LIB_WIMLIB)
$(BUILD)/src/bled_links.o: CFLAGS += -I$(VENDOR)/bled
$(BUILD)/src/iso.o: CFLAGS += -I$(LIB_SYSLINUX)/libfat
$(BUILD)/src/%.o: src/%.c $(GENERATED) | $(VENDOR)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(VENDOR)/ms-sys/%.c: $(VENDOR)/.stamp ;
$(VENDOR)/bled/%.c: $(VENDOR)/.stamp ;
$(VENDOR)/ezxml/xml.c: $(VENDOR)/.stamp ;

$(BUILD)/vendor/ezxml/%.o: $(VENDOR)/ezxml/%.c $(VENDOR)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vendor/bled/%.o: $(VENDOR)/bled/%.c $(VENDOR)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DBLED_EXPECT_DISK_IMAGE -Wno-unused-function -Wno-incompatible-pointer-types -c $< -o $@

$(BUILD)/vendor/ms-sys/%.o: $(VENDOR)/ms-sys/%.c $(GENERATED) | $(VENDOR)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# libcdio is compiled with HAVE_CONFIG_H and our Linux config.h, which must
# come before the vendored source dir (and before CFLAGS, which also adds the
# libcdio dir to the include path) in the include path
$(BUILD)/libcdio/%.o: $(LIB_CDIO)/%.c libcdio-config/config.h
	@mkdir -p $(dir $@)
	$(CC) -Ilibcdio-config $(CFLAGS) -DHAVE_CONFIG_H -I$(LIB_CDIO) -I$(LIB_CDIO)/driver -c $< -o $@

# libfat and libinstaller need their own directories in the include path
# (their headers are not on the global one)
$(BUILD)/syslinux/%.o: $(LIB_SYSLINUX)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include win32compat.h -I$(LIB_SYSLINUX)/libinstaller -I$(LIB_SYSLINUX)/libfat -Wno-address-of-packed-member -c $< -o $@

WIMLIB_CFLAGS := -O2 -g -std=gnu11 -D_GNU_SOURCE -DHAVE_CONFIG_H -DBUILDING_WIMLIB \
                 -Iwimlib-config -Ilibcdio-config -I$(LIB_WIMLIB) -I$(TOPDIR)/src/libcdio \
                 -include wimlib-config/linux_compat.h -fvisibility=hidden -Wno-unused-result -Wno-incompatible-pointer-types
$(BUILD)/wimlib/%.o: $(LIB_WIMLIB)/%.c wimlib-config/config.h
	@mkdir -p $(dir $@)
	$(CC) $(WIMLIB_CFLAGS) -c $< -o $@

$(BUILD)/wimlib/%.o: wimlib-unix/%.c wimlib-config/config.h
	@mkdir -p $(dir $@)
	$(CC) $(WIMLIB_CFLAGS) -c $< -o $@

$(BUILD)/portable/%.o: $(TOPDIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Installation: binaries, the resources Rufus uses (res/), desktop entry
PREFIX     ?= /usr/local
RES_DIRS   := dbx freedos grub grub2 icons loc mbr md5 setup syslinux uefi

install: rufus
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/rufus/res
	install -m 755 rufus $(DESTDIR)$(PREFIX)/bin/rufus
	if [ -x rufus-gui ]; then install -m 755 rufus-gui $(DESTDIR)$(PREFIX)/bin/rufus-gui; fi
	for d in $(RES_DIRS); do cp -r $(TOPDIR)/res/$$d $(DESTDIR)$(PREFIX)/share/rufus/res/; done
	install -m 644 $(TOPDIR)/res/rufus.ico $(DESTDIR)$(PREFIX)/share/rufus/res/rufus.ico
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 644 data/rufus.desktop $(DESTDIR)$(PREFIX)/share/applications/rufus.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/rufus $(DESTDIR)$(PREFIX)/bin/rufus-gui
	rm -rf $(DESTDIR)$(PREFIX)/share/rufus
	rm -f $(DESTDIR)$(PREFIX)/share/applications/rufus.desktop

test: rufus
	sh tests/run-tests.sh

clean:
	rm -rf $(BUILD) rufus rufus-gui

distclean: clean
	rm -rf $(VENDOR)
