// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Kindle books without DRM: MOBI (MOBI 6, "old" Kindle format), AZW3 (KF8)
// and joint files carrying both, in Palm database containers. Text is
// decompressed (PalmDOC LZ77 or HUFF/CDIC), turned back into HTML files and
// handed to the same converter as EPUB chapters.
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"

namespace doc {

struct MobiTocEntry {
    std::string title;
    int section = 0;
    std::string anchor;
    int level = 1;
};

class Mobi {
public:
    // Reads the container and headers (cheap); the text is decoded on first use.
    bool open(const std::string& path, std::string& error);

    const std::string& title() const { return title_; }
    const std::string& author() const { return author_; }
    bool is_kf8() const { return kf8_; }
    // The cover picture's bytes, if the book names one.
    bool cover(std::string& bytes) const;
    // A record's bytes (pictures are stored one per record).
    bool record(uint32_t index, std::string& bytes) const;

    // Decodes the text into sections (chapters); call once before sharing.
    bool prepare(std::string& error);
    int sections() const { return (int)sections_.size(); }
    size_t section_size(int i) const { return sections_[i].size(); }
    const std::vector<MobiTocEntry>& toc() const { return toc_; }
    bool load_section(int index, Document& out, std::string& error) const;

    // The image-cache key for a record of this book.
    std::string image_key(uint32_t record) const;

private:
    struct Fragment { uint32_t insert, file, start, length; };
    struct Skeleton { uint32_t fragments, start, length; };

    bool decode_text(std::string& text, std::string& error);
    void build_mobi6(std::string& text);
    void build_kf8(const std::string& text);
    void read_ncx();
    std::string resolve_kindle_pos(const std::string& href) const;

    std::string path_, title_, author_;
    std::shared_ptr<const std::string> data_;
    std::vector<uint32_t> offsets_;  // record start offsets
    uint32_t base_ = 0;              // first record of the book used (KF8 part of a joint file)
    uint32_t text_records_ = 0, compression_ = 0, text_length_ = 0, encoding_ = 0;
    uint32_t first_resource_ = 0, huff_record_ = 0, huff_count_ = 0, extra_flags_ = 0;
    uint32_t fdst_ = 0xFFFFFFFF, skeleton_index_ = 0xFFFFFFFF, fragment_index_ = 0xFFFFFFFF, ncx_index_ = 0xFFFFFFFF;
    int32_t cover_offset_ = -1;
    uint32_t kf8_boundary_hint_ = 0xFFFFFFFF;  // EXTH 121: where a joint file's KF8 part starts
    bool kf8_ = false, prepared_ = false;

    std::vector<std::string> sections_;  // UTF-8 HTML
    std::vector<uint32_t> section_start_;  // MOBI 6: byte offset where each section starts
    std::vector<std::string> flow_images_;  // SVG image wrappers -> embedded raster references
    std::vector<Fragment> fragments_;
    std::vector<Skeleton> skeletons_;
    std::vector<uint32_t> part_start_;  // KF8: text position of each part
    std::vector<MobiTocEntry> toc_;
};

// Registers the image-cache loader for "mobi:" keys (once, at startup).
void register_mobi_images();

}  // namespace doc
