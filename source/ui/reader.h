// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The reader for reflowable documents: Kiwix articles, EPUB books and text
// files. A ReaderSource supplies sections (an EPUB's chapters; an article is
// one section) and resolves links; the reader lays them out, paginates or
// scrolls, and keeps the reading position, bookmarks and history.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/narration.h"
#include "services/library.h"
#include "ui/screen.h"

namespace ui {

struct MenuItem;

struct LoadResult {
    doc::Document doc;
    std::string error;
    std::string final_url;  // articles: the address after redirects
};

struct LinkTarget {
    enum Kind { None, Section, Article, External } kind = None;
    int section = 0;
    std::string anchor;
    std::string url;
};

struct TocItem {
    std::string title;
    int section = 0;
    std::string anchor;  // empty: start of the section (or `block`)
    int block = -1;      // a block in the section, when there is no anchor
    int level = 1;
};

class ReaderSource {
public:
    virtual ~ReaderSource() = default;
    virtual std::string title() const = 0;
    // The library item this reading position belongs to.
    virtual std::string item_id() const = 0;
    virtual int sections() const { return 1; }
    virtual std::string section_title(int section) const { return std::string(); }
    // Relative length of a section, for the progress percentage.
    virtual double section_weight(int section) const { return 1; }
    // Runs on a worker thread.
    virtual bool load(int section, LoadResult& out) = 0;
    // Runs on the UI thread after a successful load.
    virtual void loaded(int section, const LoadResult& result) {}
    virtual std::vector<TocItem> toc() const { return {}; }
    virtual LinkTarget resolve(const std::string& href, int from_section) const = 0;
    virtual bool is_article() const { return false; }
    // Articles only: open another article in place (history is kept by the reader).
    virtual void navigate(const std::string& url) {}
    virtual std::string current_url() const { return std::string(); }
    // Extra entries for the reader's menu (Kiwix: search, random article).
    virtual void add_menu_items(std::vector<MenuItem>& items) {}
    // Read-along (EPUB 3 media overlays, DAISY): narration synced to the
    // text. `narration` runs on a worker; the reader then plays it, highlights
    // the passage being read and turns the pages.
    virtual bool has_narration() const { return false; }
    virtual bool narration(doc::Timeline& out, std::string& error) { return false; }
    // Start reading aloud as soon as the book opens (a talking book with no text).
    virtual bool narrate_on_open() const { return false; }

    // Set by the reader: opens another article in it, keeping history.
    std::function<void(const std::string& url)> open_url;
};

// The next book the reader opens starts reading aloud (a talking book chosen
// under Audiobooks), if it has narration.
void request_read_aloud();

// Opens the reader; it starts at the saved position of the source's item
// unless `start` is given.
void open_reader(std::unique_ptr<ReaderSource> source, const svc::Position* start = nullptr);

}  // namespace ui
