// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Read-only access to ZIP archives (EPUB, CBZ). Archives stay open in a small
// shared cache, since a book's pages and pictures are read many times; each
// archive serialises its own reads, so workers may share one.
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace doc {

struct ArchiveEntry {
    std::string name;
    uint64_t size = 0;  // uncompressed
    bool is_dir = false;
};

class Archive {
public:
    virtual ~Archive() = default;
    virtual const std::vector<ArchiveEntry>& entries() const = 0;
    // Exact name first, then a case-insensitive match (EPUBs made on Windows
    // often disagree with themselves about case).
    virtual bool read(const std::string& name, std::string& out, uint64_t max_bytes = 256ull * 1024 * 1024) = 0;
    bool has(const std::string& name) const;
    uint64_t size_of(const std::string& name) const;
    // Where an entry stored without compression lies in the archive file, so
    // it can be read in place (audio inside an EPUB). False if compressed.
    virtual bool stored_span(const std::string& name, uint64_t& offset, uint64_t& size) { return false; }
    // Writes an entry to a file, a block at a time (large compressed audio).
    virtual bool extract_to(const std::string& name, const std::string& path);
};

// Opens (or reuses) the archive at `path`. Returns nullptr if it is not a
// readable ZIP (or, with libarchive, RAR/7z) file.
std::shared_ptr<Archive> open_archive(const std::string& path, std::string* error = nullptr);
void close_archive(const std::string& path);

// Joins a path relative to the directory of `base` ("OEBPS/text/ch1.xhtml" +
// "../images/a.jpg" -> "OEBPS/images/a.jpg") and decodes %XX escapes.
std::string archive_resolve(const std::string& base, const std::string& href);

}  // namespace doc
