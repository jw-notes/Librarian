# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# Builds the PS4 package from Windows: runs tools/build.sh in WSL, then
# validates the .pkg with PkgTool and prints its SHA-256.
#   .\tools\build.ps1
param([string]$Distribution = 'kali-linux')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$linuxRoot = '/mnt/' + $root.Substring(0, 1).ToLower() + $root.Substring(2).Replace('\', '/')
& wsl -d $Distribution -- bash "$linuxRoot/tools/build.sh"
if ($LASTEXITCODE -ne 0) { throw "PS4 build failed ($LASTEXITCODE)" }
$env:DOTNET_ROLL_FORWARD = 'LatestMajor'
$version = Get-Content (Join-Path $root 'version.json') -Raw | ConvertFrom-Json
$folder = $version.release_directory
$pkg = Join-Path $root ('dist\' + $folder + '\IV0000-LIBR00001_00-LIBRARIANPS40000.pkg')
& "$root\tools\toolchain\OpenOrbis\PS4Toolchain\bin\windows\PkgTool.Core.exe" pkg_validate $pkg
if ($LASTEXITCODE -ne 0) { throw 'PKG validation failed' }
Get-FileHash -LiteralPath $pkg -Algorithm SHA256
