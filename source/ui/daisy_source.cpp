// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The reader source for DAISY talking books (doc/daisy.h): the text in
// sections, the book's navigation as contents, and its narration. A book
// with no text (headings and audio only) starts reading aloud when opened.
#include <memory>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/daisy.h"
#include "gfx/image_cache.h"
#include "platform/platform.h"
#include "services/library.h"
#include "ui/app.h"
#include "ui/reader.h"

namespace ui {

namespace {

class DaisySource : public ReaderSource {
public:
    DaisySource(const svc::Item& item, std::shared_ptr<doc::Daisy> book) : item_(item), book_(std::move(book)) {}

    std::string title() const override { return item_.title.empty() ? book_->title() : item_.title; }
    std::string item_id() const override { return item_.id; }
    int sections() const override { return book_->sections(); }
    std::string section_title(int section) const override { return book_->section_title(section); }
    double section_weight(int section) const override { return book_->section_weight(section); }
    bool load(int section, LoadResult& out) override { return book_->load_section(section, out.doc, out.error); }

    std::vector<TocItem> toc() const override {
        std::vector<TocItem> items;
        for (const auto& e : book_->toc()) items.push_back({e.title, e.section, e.anchor, -1, e.level});
        return items;
    }

    LinkTarget resolve(const std::string& href, int) const override {
        LinkTarget t;
        if (book_->resolve(href, t.section, t.anchor)) {
            t.kind = LinkTarget::Section;
        } else if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://")) {
            t.kind = LinkTarget::External;
            t.url = href;
        }
        return t;
    }

    bool has_narration() const override { return book_->has_narration(); }
    bool narration(doc::Timeline& out, std::string& error) override {
        return book_->build_narration(doc::narration_cache(lib::join_path(platform::data_root(), "narration"), item_.path),
                                      out, error);
    }
    bool narrate_on_open() const override { return !book_->has_text() && book_->has_narration(); }

private:
    svc::Item item_;
    std::shared_ptr<doc::Daisy> book_;
};

}  // namespace

void open_daisy(const svc::Item& item) {
    // A large book (hundreds of SMIL files, a long DTBook) takes a moment;
    // pressing again meanwhile does not open it twice.
    static bool opening = false;
    if (opening) return;
    opening = true;
    app().toast("Opening " + item.title + "\xE2\x80\xA6");
    const std::string path = item.path;
    lib::async<std::pair<std::shared_ptr<doc::Daisy>, std::string>>(
        nullptr,
        [path] {
            auto book = std::make_shared<doc::Daisy>();
            std::string error;
            if (!book->open(path, error)) book.reset();
            return std::make_pair(book, error);
        },
        [item](std::pair<std::shared_ptr<doc::Daisy>, std::string>& r) {
            opening = false;
            if (!r.first) {
                app().toast(r.second);
                return;
            }
            svc::Item* it = svc::library().find(item.id);
            if (it) {
                if (!r.first->title().empty() && (it->title.empty() || it->title == lib::file_stem(item.path)))
                    it->title = r.first->title();
                if (it->author.empty()) it->author = r.first->author();
                it->format = "daisy";
                svc::library().mark_dirty();
            }
            open_reader(std::make_unique<DaisySource>(it ? *it : item, r.first));
        });
}

}  // namespace ui
