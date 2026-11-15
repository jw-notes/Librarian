// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Reader sources for local books: plain text, single HTML files and word-
// processor documents (EPUB has its own source in epub_source.cpp).
#include "ui/book_sources.h"

#include "core/fs.h"
#include "core/text.h"
#include "services/local_files.h"
#include "doc/html_convert.h"
#include "doc/epub.h"
#include "doc/markdown.h"
#include "doc/office.h"
#include "doc/table.h"
#include "doc/daisy.h"
#include "gfx/image_cache.h"
#include "net/url.h"
#include "ui/app.h"
#include "ui/reader.h"

namespace ui {

void open_mobi(const svc::Item& item);
void open_fb2(const svc::Item& item);
void open_epub(const svc::Item& item);
void open_paged_document(const svc::Item& item);
void open_table_viewer(const svc::Item& item);  // table_viewer.cpp
void open_daisy(const svc::Item& item);         // daisy_source.cpp

namespace {
// A ZIP file holding a DAISY book rather than comic pages.
bool is_daisy_zip(const std::string& path) {
    std::string title, author;
    return doc::daisy_metadata(path, title, author);
}
}  // namespace

namespace {

// A text file, cut into sections of about 120 KB at paragraph breaks so that
// a very long file lays out quickly.
class TextSource : public ReaderSource {
public:
    enum class Mode { Text, Html, Markdown };
    TextSource(const svc::Item& item, Mode mode) : item_(item), mode_(mode) {
        // Read now: the section count must be known before the reader starts.
        prepare(error_);
    }

    std::string title() const override { return item_.title; }
    std::string item_id() const override { return item_.id; }
    int sections() const override { return std::max<int>(1, (int)cuts_.size() - 1); }
    double section_weight(int s) const override {
        return s + 1 < (int)cuts_.size() ? (double)(cuts_[s + 1] - cuts_[s]) : 1.0;
    }

    // Reads the file once (on the first load) and finds the section boundaries.
    bool prepare(std::string& error) {
        std::string raw;
        if (!lib::read_file(item_.path, raw, 64ull * 1024 * 1024)) {
            error = "The file could not be read.";
            return false;
        }
        // A UTF-8 byte order mark.
        if (lib::starts_with(raw, "\xEF\xBB\xBF")) raw.erase(0, 3);
        text_ = lib::to_utf8(raw);
        cuts_ = {0};
        constexpr size_t kChunk = 120 * 1024;
        while (mode_ == Mode::Text && text_.size() - cuts_.back() > kChunk * 3 / 2) {
            size_t at = text_.find("\n\n", cuts_.back() + kChunk);
            if (at == std::string::npos || at > cuts_.back() + kChunk * 2) at = text_.find('\n', cuts_.back() + kChunk);
            if (at == std::string::npos) break;
            cuts_.push_back(at + 1);
        }
        cuts_.push_back(text_.size());
        return true;
    }

    bool load(int section, LoadResult& out) override {
        if (text_.empty()) {
            out.error = error_.empty() ? "The file is empty." : error_;
            return false;
        }
        section = std::max(0, std::min(section, sections() - 1));
        const std::string part = text_.substr(cuts_[section], cuts_[section + 1] - cuts_[section]);
        if (mode_ == Mode::Html || mode_ == Mode::Markdown) {
            doc::ConvertOptions options;
            options.resolve = [](const std::string& href) { return href; };
            const std::string html = mode_ == Mode::Markdown ? doc::markdown_to_html(part) : part;
            out.doc = doc::html_to_document(html, options);
            if (out.doc.title.empty()) out.doc.title = item_.title;
        } else {
            out.doc = doc::text_document(part, item_.title);
        }
        if (out.doc.blocks.empty()) {
            out.error = "The file is empty.";
            return false;
        }
        return true;
    }

    LinkTarget resolve(const std::string& href, int) const override {
        LinkTarget t;
        if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://")) {
            t.kind = LinkTarget::External;
            t.url = href;
        } else if (!href.empty() && href[0] == '#') {
            t.kind = LinkTarget::Section;
            t.anchor = href.substr(1);
        }
        return t;
    }

private:
    svc::Item item_;
    std::string text_;
    std::vector<size_t> cuts_;
    Mode mode_ = Mode::Text;
    std::string error_;
};

// DOCX, ODT and RTF: converted to HTML sections when opened (doc/office.h).
class HtmlBookSource : public ReaderSource {
public:
    HtmlBookSource(const svc::Item& item, std::shared_ptr<doc::HtmlBook> book) : item_(item), book_(std::move(book)) {}
    ~HtmlBookSource() override {
        gfx::images_forget(doc::archive_image_key(item_.path, ""));
        gfx::images_forget("rtf:" + item_.path);
    }

    std::string title() const override { return item_.title.empty() ? book_->title : item_.title; }
    std::string item_id() const override { return item_.id; }
    int sections() const override { return std::max<int>(1, (int)book_->sections.size()); }
    double section_weight(int s) const override {
        return s < (int)book_->sections.size() ? std::max<double>(1, (double)book_->sections[s].size()) : 1.0;
    }

    std::string section_title(int section) const override {
        std::string title;
        for (const auto& e : book_->toc) {
            if (e.section > section) break;
            title = e.title;
        }
        return title;
    }

    bool load(int section, LoadResult& out) override {
        if (book_->sections.empty() || section < 0 || section >= (int)book_->sections.size()) {
            out.error = "The document is empty.";
            return false;
        }
        doc::ConvertOptions options;
        options.resolve = [](const std::string& href) { return href; };
        options.min_image = 8;
        out.doc = doc::html_to_document(book_->sections[section], options);
        if (out.doc.title.empty()) out.doc.title = title();
        if (out.doc.blocks.empty()) {
            if (book_->sections.size() == 1) {
                out.error = "The document has no text Librarian can show.";
                return false;
            }
            doc::Block b;
            b.runs.push_back({" ", 0, -1});
            out.doc.blocks.push_back(b);
        }
        return true;
    }

    std::vector<TocItem> toc() const override {
        std::vector<TocItem> items;
        for (const auto& e : book_->toc) items.push_back({e.title, e.section, e.anchor, -1, e.level});
        return items;
    }

    LinkTarget resolve(const std::string& href, int from_section) const override {
        LinkTarget t;
        if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://") || lib::starts_with(href, "mailto:")) {
            t.kind = LinkTarget::External;
            t.url = href;
        } else if (!href.empty() && href[0] == '#') {
            t.kind = LinkTarget::Section;
            t.anchor = href.substr(1);
            auto found = book_->anchors.find(t.anchor);
            t.section = found == book_->anchors.end() ? from_section : found->second;
        }
        return t;
    }

private:
    svc::Item item_;
    std::shared_ptr<doc::HtmlBook> book_;
};

void open_office_document(const svc::Item& item) {
    auto book = std::make_shared<doc::HtmlBook>();
    std::string error;
    if (!doc::open_office(item.path, *book, error)) {
        app().toast(error);
        return;
    }
    // Fill in what the library does not know yet (local files have only a file name).
    if (svc::Item* it = svc::library().find(item.id)) {
        if (it->source == "local" && !book->title.empty() && it->title == lib::file_stem(item.path)) it->title = book->title;
        if (it->author.empty()) it->author = book->author;
        if (it->cover.empty() && !book->cover.empty()) it->cover = "cover:" + item.path;
        svc::library().mark_dirty();
    }
    const svc::Item* it = svc::library().find(item.id);
    open_reader(std::make_unique<HtmlBookSource>(it ? *it : item, book));
}

}  // namespace

void open_book_file(const svc::Item& item) {
    const std::string ext = svc::format_of(item.path);
    if (ext == "epub") {
        open_epub(item);
    } else if (ext == "mobi" || ext == "azw3") {
        open_mobi(item);
    } else if (ext == "fb2") {
        open_fb2(item);
    } else if (doc::is_office_format(ext)) {
        open_office_document(item);
    } else if (doc::is_table_format(ext)) {
        open_table_viewer(item);
    } else if (ext == "daisy" || (ext == "zip" && is_daisy_zip(item.path))) {
        open_daisy(item);
    } else if (ext == "djvu" || ext == "djv") {
        app().toast("DjVu books are not supported yet.");
    } else if (ext == "md" || ext == "markdown") {
        open_reader(std::make_unique<TextSource>(item, TextSource::Mode::Markdown));
    } else if (ext == "txt") {
        open_reader(std::make_unique<TextSource>(item, TextSource::Mode::Text));
    } else if (ext == "html" || ext == "htm" || ext == "xhtml") {
        open_reader(std::make_unique<TextSource>(item, TextSource::Mode::Html));
    } else {
        open_paged_document(item);
    }
}

}  // namespace ui
