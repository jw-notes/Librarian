// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// USB drives through GoldHEN's FTP server.
//
// Inside the PS4's app sandbox /mnt/usb0..7 cannot be opened. When GoldHEN
// will not lift the sandbox (its SDK call does not answer on some firmware,
// 14.00 for one), the drives are still there for GoldHEN's own FTP server,
// which runs outside it on the console (127.0.0.1:2121). Once enable() is
// called, core/fs sends every path under the USB prefix here: folders are
// listed, files read (with seeking, through lib::open_file) and written
// (downloads) through that server. It answers only while FTP is on in
// GoldHEN's settings.
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "core/fs.h"

namespace lib::usb_ftp {

// Sends paths starting with `prefix` (e.g. "/mnt/usb") to the FTP server at
// `address` ("127.0.0.1:2121"). Once, before worker threads start.
void enable(const std::string& address, const std::string& prefix);
bool enabled();
// True when `path` goes through the server.
bool handles(const std::string& path);
std::string address();
// Connects once and says how it went, for the log and Settings.
bool reachable();
// How the last connection went: 1 answered, -1 did not, 0 not tried yet.
int answering();

bool list(const std::string& dir, std::vector<DirEntry>& out);
bool is_dir(const std::string& path);
// A file's size; `found` says whether it exists at all (a file may be empty).
uint64_t size(const std::string& path, bool* found = nullptr);
bool read_all(const std::string& path, std::string& out, uint64_t max_bytes);
bool write_all(const std::string& path, const std::string& data);
bool make_dir(const std::string& path);
bool remove_file(const std::string& path);
bool remove_dir(const std::string& path);
// Clears `to` first: what a rename over a file does on GoldHEN's server is not known.
bool rename(const std::string& from, const std::string& to);
// A read-only stream with seeking; null when the file cannot be opened.
FILE* open_read(const std::string& path);
// A new file (STOR), or more at the end of one (APPE: REST with STOR would
// empty it). fclose fails unless the server confirmed the whole transfer.
FILE* open_write(const std::string& path, bool append = false);

}  // namespace lib::usb_ftp
