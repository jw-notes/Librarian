// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// LibriVox (librivox.org): free public-domain audiobooks read by volunteers.
// Its catalogue API gives each book's chapters as MP3 files on archive.org,
// which are streamed or downloaded like an Audiobookshelf book's. Books are
// abs::Book values with ids "librivox-<n>"; they are never sent to an
// Audiobookshelf server (svc::is_local_book).
#include <string>
#include <vector>

#include "services/audiobookshelf.h"

namespace svc::librivox {

constexpr const char* kIdPrefix = "librivox-";
bool is_librivox(const std::string& id);

// Books added in the last few months, newest first (no chapters).
bool recent(std::vector<abs::Book>& out, std::string& error);
// Books whose title or author's name starts with `text` (no chapters).
bool search(const std::string& text, std::vector<abs::Book>& out, std::string& error);
// One book with its chapters (files and chapter list), by its abs::Book id.
bool book(const std::string& id, abs::Book& out, std::string& error);

// Parses the API's answer (exposed for tests).
bool parse_books(const std::string& json, std::vector<abs::Book>& out);

}  // namespace svc::librivox
