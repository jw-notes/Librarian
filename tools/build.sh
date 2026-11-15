#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# PS4 build: compile, sign (fself), assemble and package.
# Run inside WSL from anywhere; tools/build.ps1 calls it from Windows.
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="$PWD/tools/toolchain/OpenOrbis/PS4Toolchain"
export OO_PS4_TOOLCHAIN="$SDK"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
RELEASE_DIR="dist/$(python3 -c 'import json;print(json.load(open("version.json"))["release_directory"])')"
ELF="build/ps4/Librarian.elf"

make -j"$(nproc)" ps4
rm -rf build/pkg
mkdir -p build/pkg "$RELEASE_DIR"
"$SDK/bin/linux/create-fself" -in="$ELF" -out="${ELF%.elf}.oelf" --eboot build/pkg/eboot.bin \
  --paid 0x3800000000000011
python3 tools/package.py
cd build/pkg
if [[ -f /proc/sys/fs/binfmt_misc/WSLInterop ]]; then
  # The Windows PkgTool (needs the .NET runtime on Windows).
  export DOTNET_ROLL_FORWARD=LatestMajor
  export WSLENV="${WSLENV:+$WSLENV:}DOTNET_ROLL_FORWARD"
  "$SDK/bin/windows/PkgTool.Core.exe" pkg_build project.gp4 "../../$RELEASE_DIR"
else
  "$SDK/bin/linux/PkgTool.Core" pkg_build project.gp4 "../../$RELEASE_DIR"
fi
cd ../..
# The licences and Corresponding Source that must accompany the package.
python3 tools/make_source_release.py
echo "Package written to $RELEASE_DIR"
