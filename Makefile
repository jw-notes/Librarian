# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# Librarian build. Run inside WSL (or any Linux) from the project root:
#
#   make host          the Linux build used for development and tests
#   make tests         unit tests (host)
#   make ps4           eboot for the console (needs tools/toolchain, see README)
#
# PowerShell users: tools\build.ps1 runs the PS4 build and packages it.

TOOLCHAIN   ?= $(CURDIR)/tools/toolchain
OO_SDK      ?= $(TOOLCHAIN)/OpenOrbis/PS4Toolchain
LLD         ?= $(TOOLCHAIN)/llvm-local/usr/lib/llvm-18/bin/lld
PS4_CXX     ?= clang++-18
PS4_CC      ?= clang-18
HOST_CXX    ?= g++
HOST_CC     ?= gcc

APP_VERSION := $(shell python3 -c 'import json;print(json.load(open("version.json"))["app_version"])')

# ---- sources -----------------------------------------------------------------
APP_SOURCES := $(filter-out source/doc/pdf_%.cpp,$(wildcard source/core/*.cpp source/net/http.cpp \
                 source/net/url.cpp source/gfx/*.cpp source/doc/*.cpp source/audio/*.cpp source/services/*.cpp \
                 source/ui/*.cpp))

# PDF: MuPDF when its libraries have been built (tools/build_mupdf.sh host|ps4),
# otherwise PDFs are reported as unsupported.
HOST_MUPDF := $(wildcard build/mupdf-host/libmupdf.a)
PS4_MUPDF := $(wildcard build/mupdf-ps4/libmupdf.a)
HOST_PDF := $(if $(HOST_MUPDF),source/doc/pdf_mupdf.cpp,source/doc/pdf_none.cpp)
PS4_PDF := $(if $(PS4_MUPDF),source/doc/pdf_mupdf.cpp,source/doc/pdf_none.cpp)
HOST_PDF_LIBS := $(if $(HOST_MUPDF),build/mupdf-host/libmupdf.a build/mupdf-host/libmupdf-third.a)
PS4_PDF_LIBS := $(if $(PS4_MUPDF),build/mupdf-ps4/libmupdf.a build/mupdf-ps4/libmupdf-third.a)
WEBP_SOURCES := $(wildcard third_party/libwebp/src/dec/*.c third_party/libwebp/src/dsp/*.c \
                  third_party/libwebp/src/utils/*.c)
MINIZ_SOURCES := third_party/miniz/miniz.c
# USB drives (PS4 only): GoldHEN's escape through its Plugins SDK (MIT), see
# source/platform/sandbox_ps4.h; when it fails, drives go through GoldHEN's FTP
# server (source/core/usb_ftp.h).
ESCAPE_SOURCES := source/platform/sandbox_goldhen.cpp
ESCAPE_LIB_SOURCES := third_party/goldhen_sdk/source/GoldHEN.c third_party/goldhen_sdk/source/Syscall.c
PS4_BUILD := build/ps4
# libarchive: the core reader and the RAR / RAR5 formats only (CBR comics).
LA := third_party/libarchive
ARCHIVE_SOURCES := $(addprefix $(LA)/,archive_read.c archive_read_open_filename.c archive_read_set_options.c \
                   archive_read_support_format_rar.c archive_read_support_format_rar5.c archive_entry.c \
                   archive_string.c archive_string_sprintf.c archive_util.c archive_virtual.c archive_check_magic.c \
                   archive_ppmd7.c archive_blake2s_ref.c archive_blake2sp_ref.c archive_options.c archive_acl.c \
                   archive_entry_xattr.c archive_entry_sparse.c archive_rb.c archive_random.c archive_pathmatch.c \
                   archive_read_add_passphrase.c archive_cryptor.c archive_hmac.c archive_digest.c)
ARCHIVE_FLAGS := -DPLATFORM_CONFIG_H='"doc/libarchive_config.h"' -DLIBARCHIVE_STATIC -D__LIBARCHIVE_BUILD -Isource
# FAAD2 (AAC audio): its library sources with upstream's CMake configuration.
FAAD_SOURCES := $(wildcard third_party/faad2/libfaad/*.c)
FAAD_FLAGS := -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 \
              -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DAPPLY_DRC -DPACKAGE_VERSION='"2.11.4"' \
              -Ithird_party/faad2/libfaad -Ithird_party/faad2/include
# ZIM files (doc/zim.cpp): the zstd and xz decoders only.
ZSTD_SOURCES := $(addprefix third_party/zstd/,common/debug.c common/entropy_common.c common/error_private.c                 common/fse_decompress.c common/xxhash.c common/zstd_common.c decompress/huf_decompress.c                 decompress/zstd_ddict.c decompress/zstd_decompress.c decompress/zstd_decompress_block.c)
ZSTD_FLAGS := -DZSTD_DISABLE_ASM -DZSTD_TRACE=0 -DZSTD_LEGACY_SUPPORT=0 -DXXH_NAMESPACE=ZSTD_ -Ithird_party/zstd               -Ithird_party/zstd/common
XZ_SOURCES := $(addprefix third_party/xz/,xz_crc32.c xz_crc64.c xz_dec_lzma2.c xz_dec_stream.c)
XZ_FLAGS := -DXZ_USE_CRC64 -DXZ_DEC_ANY_CHECK -Ithird_party/xz
# settings.json encryption (services/secure_store.cpp).
MONO_SOURCES := third_party/monocypher/monocypher.c
# Opus audio (audio/ogg_decoder.cpp): libopus, floating point, with upstream's
# own source lists. No SIMD paths (no run-time CPU detection is configured).
-include third_party/opus/celt_sources.mk third_party/opus/silk_sources.mk third_party/opus/opus_sources.mk
OPUS_SOURCES_ALL := $(addprefix third_party/opus/,$(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FLOAT) \
                      $(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT))
OPUS_FLAGS := -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF -DHAVE_LRINT -Ithird_party/opus/include -Ithird_party/opus \
              -Ithird_party/opus/celt -Ithird_party/opus/silk -Ithird_party/opus/silk/float

INCLUDES := -I. -Isource -Ithird_party -Ithird_party/libwebp -Ithird_party/libwebp/src -Ithird_party/miniz \
            -Ithird_party/mupdf/include

# ---- host --------------------------------------------------------------------
HOST_OBJ := build/host/obj
HOST_FLAGS := -DLIB_HOST -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(INCLUDES)
HOST_APP_OBJS := $(patsubst %.cpp,$(HOST_OBJ)/%.o,$(APP_SOURCES) $(HOST_PDF) source/net/http_host.cpp \
                   source/platform/platform_host.cpp)
HOST_LIB_OBJS := $(patsubst %.c,$(HOST_OBJ)/%.o,$(WEBP_SOURCES) $(MINIZ_SOURCES) $(ARCHIVE_SOURCES) $(FAAD_SOURCES)                   $(ZSTD_SOURCES) $(XZ_SOURCES) $(MONO_SOURCES) $(OPUS_SOURCES_ALL))

$(HOST_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_FLAGS) -MMD -c $< -o $@

$(HOST_OBJ)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(INCLUDES) $(ARCHIVE_FLAGS) -c $< -o $@

$(HOST_OBJ)/third_party/faad2/%.o: third_party/faad2/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(FAAD_FLAGS) -c $< -o $@

$(HOST_OBJ)/third_party/zstd/%.o: third_party/zstd/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(ZSTD_FLAGS) -c $< -o $@

$(HOST_OBJ)/third_party/xz/%.o: third_party/xz/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(XZ_FLAGS) -c $< -o $@

$(HOST_OBJ)/third_party/opus/%.o: third_party/opus/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(OPUS_FLAGS) -c $< -o $@

build/host/librarian: $(HOST_APP_OBJS) $(HOST_LIB_OBJS) $(HOST_OBJ)/source/main.o
	$(HOST_CXX) $^ $(HOST_PDF_LIBS) -lpthread -lm -o $@

host: build/host/librarian

TEST_SOURCES := $(wildcard tests/*.cpp)
build/host/tests: $(HOST_APP_OBJS) $(HOST_LIB_OBJS) $(patsubst %.cpp,$(HOST_OBJ)/%.o,$(TEST_SOURCES))
	$(HOST_CXX) $^ $(HOST_PDF_LIBS) -lpthread -lm -o $@

tests: build/host/tests
	./build/host/tests

# ---- PS4 ---------------------------------------------------------------------
PS4_OBJ := $(PS4_BUILD)/obj
PS4_TARGET := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -isysroot $(OO_SDK) -isystem $(OO_SDK)/include
PS4_FLAGS := $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DLIB_VERSION=\"$(APP_VERSION)\" \
             -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(INCLUDES)
PS4_APP_OBJS := $(patsubst %.cpp,$(PS4_OBJ)/%.o,$(APP_SOURCES) $(PS4_PDF) source/net/http_ps4.cpp \
                  source/platform/platform_ps4.cpp $(ESCAPE_SOURCES) source/main.cpp)
PS4_LIB_OBJS := $(patsubst %.c,$(PS4_OBJ)/%.o,$(WEBP_SOURCES) $(MINIZ_SOURCES) $(ARCHIVE_SOURCES) \
                  $(ESCAPE_LIB_SOURCES) $(FAAD_SOURCES) $(ZSTD_SOURCES) $(XZ_SOURCES) $(MONO_SOURCES) $(OPUS_SOURCES_ALL))

$(PS4_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(PS4_CXX) -std=c++17 -isystem $(OO_SDK)/include/c++/v1 $(PS4_FLAGS) -fno-exceptions -MMD -c $< -o $@

$(PS4_OBJ)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -w $(INCLUDES) $(ARCHIVE_FLAGS) \
	  $(EXTRA_CFLAGS) -c $< -o $@


# GoldHEN Plugins SDK: the SDK command wrappers and its syscall stub.
$(PS4_OBJ)/third_party/goldhen_sdk/%.o: third_party/goldhen_sdk/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -w -Ithird_party/goldhen_sdk/include -c $< -o $@

$(PS4_OBJ)/third_party/faad2/%.o: third_party/faad2/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -w $(FAAD_FLAGS) -c $< -o $@

$(PS4_OBJ)/third_party/zstd/%.o: third_party/zstd/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -w $(ZSTD_FLAGS) -c $< -o $@

$(PS4_OBJ)/third_party/xz/%.o: third_party/xz/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -w $(XZ_FLAGS) -c $< -o $@

$(PS4_OBJ)/third_party/opus/%.o: third_party/opus/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -w $(OPUS_FLAGS) -c $< -o $@

# The console's headers have no <sys/disk.h>; libarchive only uses it to size
# raw disk devices, which Librarian never opens.
$(PS4_OBJ)/$(LA)/archive_read_open_filename.o: EXTRA_CFLAGS := -U__FreeBSD__

PS4_LIBS := -lc++ -lc -lm -lpthread -lclang_rt.builtins-x86_64 -lkernel -lSceLibcInternal -lSceSysmodule \
            -lSceSystemService -lSceUserService -lSceVideoOut -lScePad -lSceNet -lSceNetCtl -lSceHttp -lSceSsl \
            -lSceAudioOut

$(PS4_BUILD)/Librarian.elf: $(PS4_APP_OBJS) $(PS4_LIB_OBJS)
	$(LLD) -flavor gnu -m elf_x86_64 -pie --wrap=clock_gettime --script $(OO_SDK)/link.x --eh-frame-hdr \
	  -L$(OO_SDK)/lib $(OO_SDK)/lib/crt1.o $^ --start-group $(PS4_PDF_LIBS) $(PS4_LIBS) --end-group -o $@

ps4: $(PS4_BUILD)/Librarian.elf

clean:
	rm -rf build/host build/ps4

.PHONY: host tests ps4 clean
-include $(shell find build -name '*.d' 2>/dev/null)
