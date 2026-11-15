// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// FictionBook 2 (.fb2, .fb2.zip): an XML book format with the text, its
// structure and base64-embedded pictures in one file.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"

namespace doc {

struct Fb2TocEntry {
    std::string title;
    int section = 0;
    std::string anchor;
    int level = 1;
};

class Fb2 {
public:
    bool open(const std::string& path, std::string& error);

    const std::string& title() const { return title_; }
    const std::string& author() const { return author_; }
    const std::string& annotation() const { return annotation_; }
    int sections() const { return (int)sections_.size(); }
    size_t section_size(int i) const { return sections_[i].size(); }
    const std::vector<Fb2TocEntry>& toc() const { return toc_; }
    bool load_section(int index, Document& out, std::string& error) const;
    // The section holding element `id` (footnotes are in their own section).
    int section_of(const std::string& id) const;

    // Pictures: decoded bytes by image-cache key.
    const std::map<std::string, std::string>& images() const { return images_; }
    std::string cover_key() const { return cover_; }
    std::string image_key(const std::string& id) const;

private:
    std::string path_, title_, author_, annotation_, cover_;
    std::vector<std::string> sections_;  // HTML
    std::map<std::string, int> ids_;     // element id -> section
    std::vector<Fb2TocEntry> toc_;
    std::map<std::string, std::string> images_;
};

// Decodes text in a legacy encoding named by an XML declaration
// (windows-1251, koi8-r, windows-1252, iso-8859-1) to UTF-8.
std::string decode_legacy(const std::string& bytes, const std::string& encoding);
std::string base64_decode(const std::string& text);

}  // namespace doc
