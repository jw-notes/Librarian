#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# Builds MuPDF's static libraries for the host and/or the PS4:
#   tools/build_mupdf.sh host|ps4
# Output: build/mupdf-<target>/libmupdf.a and libmupdf-third.a.
# Only PDF rendering is kept: no JavaScript, no XPS/SVG/HTML/EPUB/CBZ handlers
# (Librarian has its own), no OCR or Office output, and no fallback CJK fonts.
set -euo pipefail
cd "$(dirname "$0")/.."
target=${1:?host or ps4}
ROOT=$PWD
SRC=third_party/mupdf
OUT="$ROOT/build/mupdf-$target"

DEFINES="-DTOFU -DTOFU_CJK -DFZ_ENABLE_XPS=0 -DFZ_ENABLE_SVG=0 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 \
 -DFZ_ENABLE_HTML=0 -DFZ_ENABLE_EPUB=0 -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0 \
 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_JS=0"

if [[ $target == ps4 ]]; then
  SDK="$ROOT/tools/toolchain/OpenOrbis/PS4Toolchain"
  # libc++'s headers must come before the C headers (MuPDF passes the C flags
  # to C++ compiles too); its C wrappers pass straight through for C files.
  TARGET_FLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -isysroot $SDK \
    -isystem $SDK/include/c++/v1 -isystem $SDK/include -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
  CC="clang-18"
  CXX="clang++-18"
  CXXFLAGS_EXTRA="-fno-exceptions -fno-rtti"
else
  TARGET_FLAGS=""
  CC="gcc"
  CXX="g++"
  CXXFLAGS_EXTRA=""
fi

cd "$SRC"
# Font and CMap data as C files (a bash hexdump; nothing compiled for the host).
make -s generate
make -j"$(nproc)" -s libs build=release OUT="$OUT" \
  CC="$CC" CXX="$CXX" AR=ar \
  XCFLAGS="$TARGET_FLAGS $DEFINES -w" XCXXFLAGS="$CXXFLAGS_EXTRA $TARGET_FLAGS $DEFINES -w" \
  HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_TESSERACT=no HAVE_LEPTONICA=no USE_SYSTEM_LIBS=no \
  shared=no verbose=no
ls -la "$OUT"/*.a
