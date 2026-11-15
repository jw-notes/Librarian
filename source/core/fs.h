// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// File helpers. Writes that must survive a crash or power cut go through
// write_file_atomic (temporary file, then rename).
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

namespace lib {

struct DirEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
    int64_t modified = 0;
    uint64_t inode = 0;  // file serial number from readdir (reliable on the console)
};

bool read_file(const std::string& path, std::string& out, uint64_t max_bytes = 512ull * 1024 * 1024);
bool write_file_atomic(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);
bool dir_exists(const std::string& path);
uint64_t file_size(const std::string& path);
bool make_dirs(const std::string& path);
bool remove_file(const std::string& path);
bool remove_dir(const std::string& path);  // an empty folder
bool rename_file(const std::string& from, const std::string& to);
// Opens a file for reading ("rb") or as a new file ("wb"). Use these, not
// fopen, for anything that may be on a USB drive: when the app could not
// leave the sandbox, those are reached through GoldHEN's FTP server
// (core/usb_ftp.h) and these streams read and write it.
FILE* open_file(const std::string& path, const char* mode = "rb");
// With `sizes` false, file sizes are left at 0 (cheaper on the console, where
// each size costs opening the file).
bool list_dir(const std::string& path, std::vector<DirEntry>& out, bool sizes = true);
std::string join_path(const std::string& a, const std::string& b);
std::string file_name(const std::string& path);
std::string file_stem(const std::string& path);
std::string file_extension(const std::string& path);  // lower-case, without the dot
std::string parent_dir(const std::string& path);
// Replaces characters FAT32/exFAT reject and bounds the length.
std::string safe_file_name(const std::string& name);
// Free bytes on the volume; 0 when unknown (always on the console).
uint64_t free_space(const std::string& path);

}  // namespace lib
