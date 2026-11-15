// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// DAISY talking books: DAISY 2.02 (ncc.html, SMIL, XHTML text, audio) and
// DAISY 3 / ANSI/NISO Z39.86 (OPF package, NCX navigation, SMIL, DTBook XML
// text, audio), in a folder or a ZIP file. The text (or, in a book without
// text, the list of its headings) is shown in the reader, split into
// sections, with the narration synced to it (doc/narration.h).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/markup.h"
#include "doc/archive.h"
#include "doc/document.h"
#include "doc/narration.h"

namespace doc {

struct DaisyTocEntry {
    std::string title;
    int section = 0;
    std::string anchor;  // element id in that section, may be empty
    int level = 1;
};

class Daisy {
public:
    // `path`: an ncc.html, a DAISY 3 .opf, or a .zip holding either.
    bool open(const std::string& path, std::string& error);

    const std::string& title() const { return title_; }
    const std::string& author() const { return author_; }
    int version() const { return version_; }       // 2 (2.02) or 3
    bool has_text() const { return has_text_; }    // full text, not only headings
    int sections() const { return (int)sections_.size(); }
    const std::string& section_title(int i) const { return sections_[i].title; }
    double section_weight(int i) const { return sections_[i].weight; }
    const std::vector<DaisyTocEntry>& toc() const { return toc_; }
    bool has_narration() const { return !points_.empty(); }

    // Converts one section (narrated passages become marks). Runs on a worker.
    bool load_section(int index, Document& out, std::string& error) const;
    // The narration on one timeline. Runs on a worker.
    bool build_narration(const std::string& cache_dir, Timeline& out, std::string& error) const;
    // A link inside the book ("daisy:<file>#<id>") -> section and anchor.
    bool resolve(const std::string& href, int& section, std::string& anchor) const;

private:
    struct Section {
        std::string file;   // the text file it comes from
        std::string title;
        double weight = 1;
        std::unique_ptr<lib::Node> root;  // a part of that file's tree
    };

    // Files of the book: a folder (names relative to it) or a ZIP archive.
    bool read(const std::string& name, std::string& out, uint64_t max = 64ull << 20) const;
    std::string real_name(const std::string& name) const;  // case-insensitive in folders
    bool locate_audio(const std::string& name, const std::string& cache_dir, audio::Track& t) const;
    std::string image_key(const std::string& name) const;

    bool open_202(const std::string& ncc, std::string& error);
    bool open_3(const std::string& opf, std::string& error);
    void parse_smils(const std::vector<std::string>& smils);
    // Parses a text file and cuts it into sections.
    void add_text_file(const std::string& name, bool dtbook);
    void assign_sections();
    void add_toc(const std::string& title, const std::string& target, int level);
    int section_of(const std::string& file, const std::string& id) const;

    std::string path_, dir_, title_, author_;
    std::shared_ptr<Archive> archive_;
    std::string archive_root_;  // folder of the package inside the ZIP
    mutable std::map<std::string, std::vector<std::string>> listings_;  // folder -> names
    int version_ = 0;
    bool has_text_ = false;
    std::vector<Section> sections_;
    std::map<std::string, int> id_section_;  // "file#id" -> section
    std::vector<SyncPoint> points_;
    std::map<std::string, int> refs_;  // "smil#id" -> passage
    std::vector<DaisyTocEntry> toc_;
};

// Whether `path` (ncc.html, .opf or .zip) is a DAISY book; its title and author.
bool daisy_metadata(const std::string& path, std::string& title, std::string& author);

}  // namespace doc
