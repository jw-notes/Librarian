// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The reader source for EPUB books.
#include <algorithm>
#include <memory>

#include "core/fs.h"
#include "core/text.h"
#include "doc/epub.h"
#include "gfx/image_cache.h"
#include "platform/platform.h"
#include "ui/app.h"
#include "ui/reader.h"

namespace ui {

namespace {

class EpubSource : public ReaderSource {
public:
    EpubSource(const svc::Item& item, std::shared_ptr<doc::Epub> epub) : item_(item), epub_(std::move(epub)) {}
    ~EpubSource() override { gfx::images_forget(doc::archive_image_key(epub_->file(), "")); }

    std::string title() const override { return item_.title.empty() ? epub_->title() : item_.title; }
    std::string item_id() const override { return item_.id; }
    int sections() const override { return (int)epub_->chapters().size(); }

    std::string section_title(int section) const override {
        // The last contents entry that starts at or before this chapter.
        const auto& chapters = epub_->chapters();
        std::string title;
        for (const auto& e : epub_->toc()) {
            const int c = epub_->chapter_of(e.path);
            if (c == section && title.empty()) return e.title;
            if (c >= 0 && c < section) title = e.title;
        }
        (void)chapters;
        return title;
    }

    double section_weight(int section) const override {
        return std::max<double>(1, (double)epub_->chapters()[section].size);
    }

    bool load(int section, LoadResult& out) override { return epub_->load_chapter(section, out.doc, out.error); }

    std::vector<TocItem> toc() const override {
        std::vector<TocItem> items;
        for (const auto& e : epub_->toc()) {
            const int c = epub_->chapter_of(e.path);
            if (c >= 0) items.push_back({e.title, c, e.anchor, -1, e.level});
        }
        if (items.empty())
            for (int i = 0; i < sections(); ++i) items.push_back({"Chapter " + std::to_string(i + 1), i, "", -1, 1});
        return items;
    }

    LinkTarget resolve(const std::string& href, int) const override {
        LinkTarget t;
        if (lib::starts_with(href, "epub:")) {
            const std::string target = href.substr(5);
            const size_t hash = target.find('#');
            const int c = epub_->chapter_of(hash == std::string::npos ? target : target.substr(0, hash));
            if (c >= 0) {
                t.kind = LinkTarget::Section;
                t.section = c;
                t.anchor = hash == std::string::npos ? "" : target.substr(hash + 1);
            }
        } else if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://")) {
            t.kind = LinkTarget::External;
            t.url = href;
        }
        return t;
    }

    bool has_narration() const override { return epub_->has_narration(); }
    bool narration(doc::Timeline& out, std::string& error) override {
        return epub_->build_narration(
            doc::narration_cache(lib::join_path(platform::data_root(), "narration"), epub_->file()), out, error);
    }

private:
    svc::Item item_;
    std::shared_ptr<doc::Epub> epub_;
};

}  // namespace

void open_epub(const svc::Item& item) {
    auto epub = std::make_shared<doc::Epub>();
    std::string error;
    if (!epub->open(item.path, error)) {
        app().toast(error);
        return;
    }
    // Fill in what the library does not know yet (local files have only a file name).
    svc::Item* it = svc::library().find(item.id);
    if (it) {
        if (it->source == "local" && !epub->title().empty() && it->title == lib::file_stem(item.path)) it->title = epub->title();
        if (it->author.empty()) it->author = epub->author();
        if (it->cover.empty() && !epub->cover_path().empty()) it->cover = epub->image_key(epub->cover_path());
        svc::library().mark_dirty();
    }
    open_reader(std::make_unique<EpubSource>(it ? *it : item, epub));
}

}  // namespace ui
