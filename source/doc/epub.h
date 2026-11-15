// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// EPUB 2 and 3: container, package (OPF) metadata, spine, cover and table of
// contents (EPUB 3 nav document or EPUB 2 NCX). Chapters are converted with
// the same HTML converter as Kiwix articles.
#include <memory>
#include <string>
#include <vector>

#include "doc/archive.h"
#include "doc/document.h"
#include "doc/narration.h"

namespace doc {

struct EpubChapter {
    std::string path;  // inside the archive
    uint64_t size = 0;
    std::string overlay;  // EPUB 3 media overlay (SMIL) narrating it, may be empty
};

struct EpubTocEntry {
    std::string title;
    std::string path;    // inside the archive
    std::string anchor;  // fragment, may be empty
    int level = 1;
};

class Epub {
public:
    bool open(const std::string& file, std::string& error);

    const std::string& title() const { return title_; }
    const std::string& author() const { return author_; }
    const std::string& language() const { return language_; }
    const std::string& description() const { return description_; }
    const std::string& cover_path() const { return cover_; }  // inside the archive, may be empty
    const std::vector<EpubChapter>& chapters() const { return chapters_; }
    const std::vector<EpubTocEntry>& toc() const { return toc_; }
    std::shared_ptr<Archive> archive() const { return archive_; }
    const std::string& file() const { return file_; }

    int chapter_of(const std::string& path) const;
    // Converts one chapter. Image sources become image-cache keys
    // (see image_key); links become "epub:<path>#<anchor>".
    bool load_chapter(int index, Document& out, std::string& error) const;

    // The image-cache key for a file inside this book.
    std::string image_key(const std::string& path) const;

    // Read-along (EPUB 3 media overlays): whether any chapter is narrated, and
    // the whole book's narration on one timeline. Audio stored uncompressed is
    // played from inside the book; compressed audio is extracted into
    // `cache_dir` first. Runs on a worker.
    bool has_narration() const;
    bool build_narration(const std::string& cache_dir, Timeline& out, std::string& error) const;

private:
    void parse_ncx(const std::string& path);
    void parse_nav(const std::string& path);
    // The passages of one chapter's overlay.
    void overlay_points(int chapter, std::vector<SyncPoint>& out) const;

    std::string file_, title_, author_, language_, description_, cover_;
    std::shared_ptr<Archive> archive_;
    std::vector<EpubChapter> chapters_;
    std::vector<EpubTocEntry> toc_;
};

// Registers the image-cache loader for "archive:" keys (once, at startup).
void register_archive_images();
// The cache key for `inner` inside the archive at `file`.
std::string archive_image_key(const std::string& file, const std::string& inner);

}  // namespace doc
