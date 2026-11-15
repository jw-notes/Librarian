// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Reader sources for Kindle books (MOBI, AZW3) and FictionBook (FB2).
#include <algorithm>
#include <memory>

#include "core/fs.h"
#include "core/text.h"
#include "doc/fb2.h"
#include "doc/mobi.h"
#include "gfx/image_cache.h"
#include "ui/app.h"
#include "ui/reader.h"

namespace ui {

namespace {

// "mobi:3#anchor" / "fb2:3#anchor" -> section 3, anchor.
LinkTarget section_link(const std::string& href, const std::string& scheme) {
    LinkTarget t;
    if (lib::starts_with(href, scheme)) {
        const std::string rest = href.substr(scheme.size());
        const size_t hash = rest.find('#');
        t.kind = LinkTarget::Section;
        t.section = atoi(rest.c_str());
        t.anchor = hash == std::string::npos ? "" : rest.substr(hash + 1);
    } else if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://")) {
        t.kind = LinkTarget::External;
        t.url = href;
    }
    return t;
}

class MobiSource : public ReaderSource {
public:
    MobiSource(const svc::Item& item, std::shared_ptr<doc::Mobi> book) : item_(item), book_(std::move(book)) {}
    ~MobiSource() override { gfx::images_forget("mobi:" + item_.path); }

    std::string title() const override { return item_.title.empty() ? book_->title() : item_.title; }
    std::string item_id() const override { return item_.id; }
    int sections() const override { return std::max(1, book_->sections()); }
    double section_weight(int s) const override {
        return s < book_->sections() ? std::max<double>(1, (double)book_->section_size(s)) : 1;
    }
    bool load(int section, LoadResult& out) override { return book_->load_section(section, out.doc, out.error); }
    std::vector<TocItem> toc() const override {
        std::vector<TocItem> items;
        for (const auto& e : book_->toc()) items.push_back({e.title, e.section, e.anchor, -1, e.level});
        return items;
    }
    LinkTarget resolve(const std::string& href, int) const override { return section_link(href, "mobi:"); }

private:
    svc::Item item_;
    std::shared_ptr<doc::Mobi> book_;
};

class Fb2Source : public ReaderSource {
public:
    Fb2Source(const svc::Item& item, std::shared_ptr<doc::Fb2> book) : item_(item), book_(std::move(book)) {
        // Pictures are already decoded in memory; the image cache asks for them by key.
        for (const auto& [key, bytes] : book_->images()) {
            std::shared_ptr<doc::Fb2> keep = book_;
            const std::string k = key;
            gfx::image_register_loader(key, [keep, k](std::string& out) {
                auto found = keep->images().find(k);
                if (found == keep->images().end()) return false;
                out = found->second;
                return true;
            });
        }
    }
    ~Fb2Source() override { gfx::images_forget("fb2:" + item_.path); }

    std::string title() const override { return item_.title.empty() ? book_->title() : item_.title; }
    std::string item_id() const override { return item_.id; }
    int sections() const override { return book_->sections(); }
    double section_weight(int s) const override { return std::max<double>(1, (double)book_->section_size(s)); }
    bool load(int section, LoadResult& out) override { return book_->load_section(section, out.doc, out.error); }
    std::vector<TocItem> toc() const override {
        std::vector<TocItem> items;
        for (const auto& e : book_->toc()) items.push_back({e.title, e.section, e.anchor, -1, e.level});
        return items;
    }
    LinkTarget resolve(const std::string& href, int) const override { return section_link(href, "fb2:"); }

private:
    svc::Item item_;
    std::shared_ptr<doc::Fb2> book_;
};

void fill_item(const svc::Item& item, const std::string& title, const std::string& author, const std::string& cover) {
    svc::Item* it = svc::library().find(item.id);
    if (!it) return;
    if (it->source == "local" && !title.empty() && it->title == lib::file_stem(item.path)) it->title = title;
    if (it->author.empty()) it->author = author;
    if (it->cover.empty() && !cover.empty()) it->cover = cover;
    svc::library().mark_dirty();
}

}  // namespace

void open_mobi(const svc::Item& item) {
    auto book = std::make_shared<doc::Mobi>();
    std::string error;
    // Decoding the text takes a moment for a long book; it is done here once so
    // the reader knows its sections.
    if (!book->open(item.path, error) || !book->prepare(error)) {
        app().toast(error);
        return;
    }
    std::string cover_bytes;
    fill_item(item, book->title(), book->author(), book->cover(cover_bytes) ? "cover:" + item.path : "");
    const svc::Item* it = svc::library().find(item.id);
    open_reader(std::make_unique<MobiSource>(it ? *it : item, book));
}

void open_fb2(const svc::Item& item) {
    auto book = std::make_shared<doc::Fb2>();
    std::string error;
    if (!book->open(item.path, error)) {
        app().toast(error);
        return;
    }
    fill_item(item, book->title(), book->author(), book->cover_key().empty() ? "" : "cover:" + item.path);
    const svc::Item* it = svc::library().find(item.id);
    open_reader(std::make_unique<Fb2Source>(it ? *it : item, book));
}

}  // namespace ui
