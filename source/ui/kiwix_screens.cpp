// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The Knowledge tab: Kiwix collections, a collection's page, search, and the
// reader source for articles.
#include <pthread.h>
#include <time.h>

#include <algorithm>

#include "core/fs.h"
#include "core/text.h"
#include "gfx/font.h"
#include "gfx/image_cache.h"
#include "net/url.h"
#include "services/kiwix.h"
#include "services/library.h"
#include "services/local_library.h"
#include "services/settings.h"
#include "services/zim_shelf.h"
#include "ui/app.h"
#include "ui/kiwix_screens.h"
#include "ui/openers.h"
#include "ui/overlays.h"
#include "ui/reader.h"
#include "ui/troubleshoot.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

std::string server() { return lib::normalise_server_url(svc::settings().kiwix_url); }

// The catalogue is kept between visits to the tab; Triangle refreshes it.
struct CatalogCache {
    std::vector<svc::ZimBook> books;  // the server's
    std::string server;
    bool loaded = false;
    std::vector<svc::ZimBook> local;  // ZIM files on the console and USB drives
    uint64_t local_generation = ~0ull;
};
CatalogCache& catalog_cache() {
    static CatalogCache cache;
    return cache;
}

const svc::ZimBook* find_book(const std::string& content) {
    for (const auto& b : catalog_cache().local)
        if (b.content == content) return &b;
    for (const auto& b : catalog_cache().books)
        if (b.content == content) return &b;
    return nullptr;
}

// Where a collection is read from: the in-process server for local files.
std::string server_for(const std::string& content) {
    const svc::ZimBook* b = find_book(content);
    return b && !b->server.empty() ? b->server : server();
}

std::string origin_of(const std::string& url) {
    lib::Url u;
    return lib::parse_url(url, u) ? u.origin() : server();
}

std::string language_name(const std::string& code) {
    static const std::pair<const char*, const char*> names[] = {
        {"eng", "English"}, {"fra", "French"},   {"deu", "German"},  {"spa", "Spanish"}, {"ita", "Italian"},
        {"por", "Portuguese"}, {"nld", "Dutch"}, {"rus", "Russian"}, {"jpn", "Japanese"}, {"zho", "Chinese"},
        {"ara", "Arabic"},  {"pol", "Polish"},   {"swe", "Swedish"}, {"mul", "Multilingual"}};
    for (const auto& [c, n] : names)
        if (code == c) return n;
    return code;
}

std::string count_label(const svc::ZimBook& b) {
    std::string meta = lib::format_count(b.articles) + " articles";
    if (!b.language.empty()) meta += "  \xC2\xB7  " + language_name(b.language);
    return meta;
}

// ---- article source --------------------------------------------------------------

class KiwixSource : public ReaderSource {
public:
    KiwixSource(std::string url, std::string collection_title)
        : url_(std::move(url)), collection_title_(std::move(collection_title)) {
        svc::kiwix::split_article_url(url_, content_, path_);
        if (collection_title_.empty())
            if (const svc::ZimBook* b = find_book(content_)) collection_title_ = b->title;
        if (const svc::Item* item = svc::library().find(item_id())) title_ = item->title;
    }

    std::string title() const override { return title_.empty() ? collection_title_ : title_; }
    std::string item_id() const override { return path_.empty() ? std::string() : svc::article_id(content_, path_); }
    bool is_article() const override { return true; }
    std::string current_url() const override { return url_; }

    bool load(int, LoadResult& out) override {
        pthread_mutex_lock(&lock_);
        const std::string url = url_;
        pthread_mutex_unlock(&lock_);
        svc::Article article;
        if (!svc::kiwix::fetch_article(url, article, out.error)) return false;
        out.doc = std::move(article.doc);
        out.final_url = article.url;
        return true;
    }

    void loaded(int, const LoadResult& r) override {
        pthread_mutex_lock(&lock_);
        url_ = lib::strip_fragment(r.final_url);
        pthread_mutex_unlock(&lock_);
        svc::kiwix::split_article_url(url_, content_, path_);
        title_ = r.doc.title.empty() ? path_ : r.doc.title;
        if (path_.empty()) return;  // not a /content/ address: nothing to remember
        svc::Item item;
        item.id = item_id();
        item.kind = svc::ItemKind::Article;
        item.source = "kiwix";
        item.title = title_;
        item.url = url_;
        item.collection = content_;
        item.collection_title = collection_title_;
        if (const svc::ZimBook* b = find_book(content_)) item.cover = b->icon;
        svc::library().upsert(item);
        svc::library().touch(item.id);
    }

    void navigate(const std::string& url) override {
        pthread_mutex_lock(&lock_);
        url_ = url;
        pthread_mutex_unlock(&lock_);
        title_.clear();
    }

    LinkTarget resolve(const std::string& href, int) const override {
        LinkTarget t;
        const std::string srv = origin_of(url_);
        if (svc::kiwix::is_article_link(srv, href)) {
            if (lib::strip_fragment(href) == lib::strip_fragment(url_)) {
                t.kind = LinkTarget::Section;
                const size_t hash = href.find('#');
                t.anchor = hash == std::string::npos ? "" : lib::url_decode(href.substr(hash + 1));
            } else {
                t.kind = LinkTarget::Article;
                t.url = href;
            }
        } else if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://")) {
            t.kind = LinkTarget::External;
            t.url = href;
        }
        return t;
    }

    void add_menu_items(std::vector<MenuItem>& items) override {
        const std::string content = content_, title = collection_title_;
        items.push_back({"Search " + (title.empty() ? std::string("this collection") : title), nullptr,
                         [content, title] { open_kiwix_search(content, title); }, nullptr});
        items.push_back({"Random article", nullptr, [this, content] {
                             if (open_url) open_url(svc::kiwix::random_url(origin_of(url_), content));
                         },
                         nullptr});
        items.push_back({"Collection main page", nullptr, [this, content] {
                             if (open_url) open_url(origin_of(url_) + "/content/" + content);
                         },
                         nullptr});
    }

private:
    mutable pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
    std::string url_, content_, path_, title_, collection_title_;
};

void draw_collection_icon(gfx::Canvas& c, const Rect& r, const svc::ZimBook& b) {
    const Theme& t = theme();
    c.fill_rounded(r, 14, gfx::mix(t.surface_hi, gfx::rgb(0xFFFFFF), 20));
    const int s = r.w * 2 / 3;
    if (const gfx::Image* img = b.icon.empty() ? nullptr : gfx::image_get(b.icon, s, s, gfx::Fit::Contain)) {
        c.draw_image(*img, r.x + (r.w - img->lw()) / 2, r.y + (r.h - img->lh()) / 2);
    } else {
        const std::string letter = b.title.empty() ? "?" : b.title.substr(0, lib::utf8_prefix(b.title, 1));
        const int px = r.h / 2;
        const int w = gfx::text_width(Face::SansBold, px, letter);
        gfx::draw_text(c, Face::SansBold, px, r.x + (r.w - w) / 2, r.y + (r.h - gfx::font_metrics(Face::SansBold, px).line_height) / 2,
                       letter, t.muted);
    }
}

// ---- the Knowledge tab ---------------------------------------------------------------

class KnowledgeScreen : public Screen {
public:
    KnowledgeScreen() {
        if (!server().empty() && (!catalog_cache().loaded || catalog_cache().server != server())) refresh();
        // ZIM files on the console and USB drives (a quick look; files seen
        // before are not opened again).
        svc::zim_shelf().scan_async();
        sync_local();
        local_generation_ = svc::local_library().generation();
    }

    Tab tab() const override { return Tab::Knowledge; }
    bool animating() const override { return loading_ || svc::zim_shelf().scanning(); }

    std::vector<Hint> hints() const override {
        if (setup()) return {{kSquare, "Kiwix online library"}, {kCross, "Set up Kiwix"}};
        const auto books = cards();
        if (books.empty() && !error_.empty() && !loading_)
            return {{kSquare, "Troubleshoot"}, {kCross, "Try again"}, {kOptions, "Online library"}};
        std::vector<Hint> h = {{kTriangle, "Refresh"}, {kOptions, "Online library"}};
        if (!books.empty()) {
            if (nav_.index > 0) h.insert(h.begin(), {kSquare, "Search"});
            h.insert(h.begin(), {kCross, "Open"});
        }
        return h;
    }

    void update(const Input& in) override {
        // Files found, deleted or downloaded; a drive plugged in or out.
        if (svc::zim_shelf().generation() != catalog_cache().local_generation) sync_local();
        if (svc::local_library().generation() != local_generation_) {
            local_generation_ = svc::local_library().generation();
            svc::zim_shelf().scan_async();
        }
        if (setup()) {
            if (in.press(kSquare)) {
                // Shown from now on, also when this tab is opened again.
                svc::settings().kiwix_online = true;
                svc::settings_save();
                app().switch_tab(Tab::Knowledge);
                return;
            }
            if (in.press(kCross)) {
                open_keyboard("Kiwix Server address (e.g. 192.168.1.10:8080)", "http://", KeyboardKind::Url, [this](const std::string& v) {
                    svc::settings().kiwix_url = lib::normalise_server_url(v);
                    svc::settings_save();
                    refresh();
                });
            }
            return;
        }
        if (in.press(kTriangle)) {
            if (!server().empty()) refresh();
            svc::zim_shelf().scan_async();
        }
        if (in.press(kOptions)) {
            open_kiwix_online_library();
            return;
        }
        const auto books = cards();
        if (books.empty()) {
            if (in.press(kCross) && !loading_ && !server().empty()) refresh();
            if (in.press(kSquare) && !error_.empty()) open_troubleshooter(Service::Kiwix, status_, error_);
            return;
        }
        // The first card is the online library, then the collections.
        nav_.update(in, (int)books.size() + 1, kCols, kRows);
        if (nav_.index == 0) {
            if (in.press(kCross)) open_kiwix_online_library();
            return;
        }
        const svc::ZimBook& b = *books[nav_.index - 1];
        if (in.press(kCross)) open_kiwix_collection(b);
        if (in.press(kSquare)) open_kiwix_search(b.content, b.title);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        const Rect area{kMarginX, kContentTop, kContentW, kFooterTop - kContentTop};
        if (setup()) {
            draw_message(c, area, "Connect to Kiwix",
                         "Librarian reads Wikipedia and other ZIM collections from your Kiwix Server, or from ZIM files "
                         "on a USB drive (in its top folder, or Librarian/Kiwix) or downloaded to this PS4. Press X to "
                         "enter your server's address. Square opens the Kiwix online library, where collections can be "
                         "found and downloaded.");
            return;
        }
        const auto books = cards();
        if (books.empty()) {
            if (loading_ || svc::zim_shelf().scanning()) {
                draw_loading(c, area, server().empty() ? std::string("Looking for ZIM files") : "Loading collections from " + server());
            } else if (error_.empty()) {
                draw_message(c, area, "No collections", "The server lists no ZIM files.");
            } else {
                const Diagnosis d = diagnose(Service::Kiwix, status_, error_);
                draw_message(c, area, d.title.empty() ? "Could not load the collections" : d.title,
                             error_ + " Press Square for troubleshooting steps, or X to try again.", true);
            }
            return;
        }
        gfx::draw_text(c, Face::SansBold, kTitlePx, kMarginX, kContentTop, "Knowledge", t.text);
        std::string count;
        const size_t local = catalog_cache().local.size(), remote = books.size() - local;
        if (remote) count = std::to_string(remote) + " on " + server();
        else if (!server().empty() && !error_.empty() && !loading_) count = "Your server did not answer";
        if (local) count += (count.empty() ? "" : "  \xC2\xB7  ") + std::to_string(local) + " on this PS4 and USB drives";
        gfx::draw_text(c, Face::Sans, kSmallPx, kMarginX + gfx::text_width(Face::SansBold, kTitlePx, "Knowledge") + 28,
                       kContentTop + 18, count, t.muted);

        const int gap = 24, card_w = (kContentW - (kCols - 1) * gap) / kCols, card_h = 222;
        const int top = kContentTop + 84;
        const int count_all = (int)books.size() + 1;
        for (int i = nav_.top_row * kCols; i < std::min(count_all, (nav_.top_row + kRows) * kCols); ++i) {
            const int row = i / kCols - nav_.top_row, col = i % kCols;
            const Rect r{kMarginX + col * (card_w + gap), top + row * (card_h + gap), card_w, card_h};
            const bool on = i == nav_.index;
            if (i == 0) {
                online_card(c, r, on);
                continue;
            }
            const svc::ZimBook& b = *books[i - 1];
            draw_panel(c, r, on);
            if (on) draw_focus(c, r, 14);
            draw_collection_icon(c, {r.x + 24, r.y + 24, 96, 96}, b);
            const int tx = r.x + 144, tw = r.w - 168;
            const auto title = gfx::wrap_text(Face::SansSemiBold, 29, b.title, tw, 2);
            int y = r.y + 26;
            for (const auto& line : title) {
                gfx::draw_text(c, Face::SansSemiBold, 29, tx, y, line, t.text);
                y += 38;
            }
            std::string meta = count_label(b);
            if (b.local) meta = std::string(b.on_usb ? "USB drive" : "This PS4") + "  \xC2\xB7  " + meta;
            gfx::draw_text(c, Face::Sans, 21, tx, y + 4, gfx::ellipsize(Face::Sans, 21, meta, tw), b.local ? t.accent : t.muted);
            gfx::draw_wrapped(c, Face::Sans, 22, r.x + 24, r.y + 138, r.w - 48, b.summary, t.muted, 2, 31);
        }
        const int total_rows = (count_all + kCols - 1) / kCols;
        draw_scrollbar(c, {kWidth - kMarginX + 30, top, 8, kRows * (card_h + gap) - gap}, nav_.top_row, kRows, total_rows);
    }

private:
    static constexpr int kCols = 3, kRows = 3;

    // Nothing to show yet: no server, and no ZIM file found.
    bool setup() const { return server().empty() && catalog_cache().local.empty() && !svc::zim_shelf().scanning(); }

    // Local files first, then the server's collections.
    std::vector<const svc::ZimBook*> cards() const {
        std::vector<const svc::ZimBook*> out;
        for (const auto& b : catalog_cache().local) out.push_back(&b);
        if (!server().empty())
            for (const auto& b : catalog_cache().books) out.push_back(&b);
        return out;
    }

    void sync_local() {
        catalog_cache().local = svc::zim_shelf().books();
        catalog_cache().local_generation = svc::zim_shelf().generation();
        nav_.clamp((int)cards().size() + 1, kCols, kRows);
    }

    static void online_card(gfx::Canvas& c, const Rect& r, bool on) {
        const Theme& t = theme();
        draw_panel(c, r, on);
        if (on) draw_focus(c, r, 14);
        const Rect icon{r.x + 24, r.y + 24, 96, 96};
        c.fill_rounded(icon, 14, t.accent);
        const char* glyph = "+";
        gfx::draw_text(c, Face::SansBold, 64, icon.x + (icon.w - gfx::text_width(Face::SansBold, 64, glyph)) / 2,
                       icon.y + (icon.h - gfx::font_metrics(Face::SansBold, 64).line_height) / 2, glyph, t.accent_text);
        gfx::draw_text(c, Face::SansSemiBold, 29, r.x + 144, r.y + 26, "Kiwix online library", t.text);
        gfx::draw_text(c, Face::Sans, 21, r.x + 144, r.y + 68, "library.kiwix.org", t.muted);
        gfx::draw_wrapped(c, Face::Sans, 22, r.x + 24, r.y + 138, r.w - 48,
                          "Find more collections, download them, or send them to your server.", t.muted, 2, 31);
    }

    void refresh() {
        loading_ = true;
        error_.clear();
        const std::string srv = server();
        struct Result {
            std::vector<svc::ZimBook> books;
            std::string error;
            int status = -1;
        };
        lib::async<Result>(
            token_,
            [srv]() {
                Result r;
                svc::kiwix::catalog(srv, r.books, r.error, &r.status);
                return r;
            },
            [this, srv](Result& r) {
                loading_ = false;
                error_ = r.error;
                status_ = r.status;
                CatalogCache& cache = catalog_cache();
                if (!r.books.empty() || cache.server != srv) cache.books = std::move(r.books);
                cache.server = srv;
                cache.loaded = true;
                nav_.clamp((int)cards().size() + 1, kCols, kRows);
            });
    }

    GridNav nav_;
    bool loading_ = false;
    std::string error_;
    int status_ = 0;
    uint64_t local_generation_ = 0;
};

// ---- a collection's page ------------------------------------------------------------

class CollectionScreen : public Screen {
public:
    explicit CollectionScreen(svc::ZimBook book) : book_(std::move(book)) { reload(); }

    Tab tab() const override { return Tab::Knowledge; }
    void resumed() override { reload(); }

    std::vector<Hint> hints() const override {
        return {{kCircle, "Back"}, {kCross, "Open"}, {kSquare, "Search"}};
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        nav_.update(in, (int)rows_.size(), kVisible);
        if (in.press(kSquare)) open_kiwix_search(book_.content, book_.title);
        if (in.press(kCross) && !rows_.empty()) rows_[nav_.index].action();
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        // Left: the collection.
        const Rect info{kMarginX, kContentTop, 560, kFooterTop - kContentTop - 30};
        draw_panel(c, info);
        draw_collection_icon(c, {info.x + 40, info.y + 40, 128, 128}, book_);
        int y = info.y + 200;
        y += gfx::draw_wrapped(c, Face::SansBold, 38, info.x + 40, y, info.w - 80, book_.title, t.text, 3) + 12;
        y += gfx::draw_wrapped(c, Face::Sans, 24, info.x + 40, y, info.w - 80, book_.summary, t.muted, 4, 34) + 24;
        auto fact = [&](const std::string& k, const std::string& v) {
            if (v.empty()) return;
            gfx::draw_text(c, Face::Sans, 22, info.x + 40, y, k, t.faint);
            gfx::draw_text(c, Face::SansSemiBold, 22, info.x + 230, y, gfx::ellipsize(Face::SansSemiBold, 22, v, info.w - 270), t.text);
            y += 38;
        };
        fact("Articles", lib::format_count(book_.articles));
        if (book_.local) {
            fact("Stored on", book_.on_usb ? "USB drive" : "This PS4");
            fact("Size", lib::format_bytes(book_.size));
        }
        if (book_.media) fact("Media", lib::format_count(book_.media));
        fact("Language", language_name(book_.language));
        fact("Updated", book_.updated);
        fact("Variant", book_.flavour);

        // Right: actions and articles.
        const int lx = info.right() + 40, lw = kWidth - kMarginX - lx;
        int ly = kContentTop;
        for (int i = nav_.top; i < std::min((int)rows_.size(), nav_.top + kVisible); ++i) {
            const Row& row = rows_[i];
            if (row.header) {
                gfx::draw_text(c, Face::SansSemiBold, 24, lx + 8, ly + 26, row.title, t.muted);
                ly += kRowH;
                continue;
            }
            const Rect r{lx, ly, lw, kRowH - 12};
            const bool on = i == nav_.index;
            draw_panel(c, r, on);
            if (on) draw_focus(c, r, 14);
            gfx::draw_text(c, row.action_row ? Face::SansSemiBold : Face::Sans, 28, r.x + 28, r.y + 10,
                           gfx::ellipsize(Face::Sans, 28, row.title, r.w - 56), t.text);
            if (!row.detail.empty())
                gfx::draw_text(c, Face::Sans, 21, r.x + 28, r.y + 48, gfx::ellipsize(Face::Sans, 21, row.detail, r.w - 56), t.muted);
            ly += kRowH;
        }
        draw_scrollbar(c, {kWidth - kMarginX + 30, kContentTop, 8, kVisible * kRowH}, nav_.top, kVisible, (int)rows_.size());
    }

private:
    struct Row {
        std::string title, detail;
        std::function<void()> action;
        bool header = false, action_row = false;
    };
    static constexpr int kRowH = 96, kVisible = 8;

    void reload() {
        rows_.clear();
        const svc::ZimBook b = book_;
        const std::string srv = b.server.empty() ? server() : b.server;
        rows_.push_back({"Main page", "Start at the collection's home page",
                         [b, srv] { open_article(svc::kiwix::main_page_url(srv, b), b.title); }, false, true});
        rows_.push_back({"Search", b.local ? "Find articles by title" : "Find articles by title or full text",
                         [b] { open_kiwix_search(b.content, b.title); }, false, true});
        rows_.push_back({"Random article", "Open something at random",
                         [b, srv] { open_article(svc::kiwix::random_url(srv, b.content), b.title); }, false, true});
        if (b.local) {
            rows_.push_back({"Delete this file", b.file, [this, b] {
                                 confirm("Delete " + b.title + "?",
                                         "The file " + lib::file_name(b.file) + " (" + lib::format_bytes(b.size) +
                                             ") is deleted from " + (b.on_usb ? "the USB drive." : "this PS4."),
                                         "Delete", [this, b] { remove(b); });
                             },
                             false, true});
        }
        const auto saved = svc::library().article_bookmarks(book_.content);
        if (!saved.empty()) {
            rows_.push_back({"Saved articles", "", nullptr, true});
            for (const svc::Item* it : saved) add_article(*it);
        }
        const auto recent = svc::library().recent_articles(30, book_.content);
        if (!recent.empty()) {
            rows_.push_back({"Recently read", "", nullptr, true});
            for (const svc::Item* it : recent) add_article(*it);
        }
        // Keep focus off headers.
        nav_.clamp((int)rows_.size(), kVisible);
    }

    void add_article(const svc::Item& it) {
        const svc::Item copy = it;
        std::string detail = it.favourite ? "Favourite" : "";
        if (!it.bookmarks.empty()) detail += (detail.empty() ? "" : "  \xC2\xB7  ") + std::to_string(it.bookmarks.size()) + " bookmark" + (it.bookmarks.size() == 1 ? "" : "s");
        if (it.progress > 0.02f) detail += (detail.empty() ? "" : "  \xC2\xB7  ") + std::to_string((int)(it.progress * 100)) + "% read";
        rows_.push_back({it.title, detail, [copy] { open_item(copy); }});
    }

    void remove(const svc::ZimBook& b) {
        app().toast("Deletingâ¦");
        const std::string content = b.content;
        lib::async<std::string>(
            token_,
            [content]() {
                std::string error;
                return svc::zim_shelf().remove(content, error) ? std::string() : error;
            },
            [](std::string& error) {
                app().toast(error.empty() ? std::string("Deleted") : error);
                if (error.empty()) app().pop();
            });
    }

    svc::ZimBook book_;
    std::vector<Row> rows_;
    ListNav nav_;
};

// ---- search ---------------------------------------------------------------------------

class SearchScreen : public Screen {
public:
    SearchScreen(std::string content, std::string title) : content_(std::move(content)), title_(std::move(title)) {}

    Tab tab() const override { return Tab::Knowledge; }
    bool animating() const override { return busy_ > 0; }

    void ask() {
        open_keyboard("Search " + title_, query_, KeyboardKind::Text, [this](const std::string& q) {
            if (lib::trim(q).empty()) {
                if (query_.empty()) app().pop();
                return;
            }
            query_ = lib::trim(q);
            run();
        });
    }

    std::vector<Hint> hints() const override {
        return {{kCircle, "Back"}, {kCross, "Open"}, {kSquare, "New search"}};
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (in.press(kSquare)) {
            ask();
            return;
        }
        const int count = (int)rows_.size();
        nav_.update(in, count, kVisible);
        if (in.press(kCross) && count) {
            const Row& r = rows_[nav_.index];
            if (r.more) more();
            else if (!r.url.empty()) open_article(r.url, title_);
        }
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        const Rect area{kMarginX, kContentTop, kContentW, kFooterTop - kContentTop};
        if (query_.empty()) {
            draw_message(c, area, "Search " + title_, "Press Square to type what you are looking for.");
            return;
        }
        gfx::draw_text(c, Face::SansBold, 40, kMarginX, kContentTop, gfx::ellipsize(Face::SansBold, 40, "\xE2\x80\x9C" + query_ + "\xE2\x80\x9D", 1000), t.text);
        std::string sub = title_;
        if (total_ > 0) sub += "  \xC2\xB7  " + lib::format_count(total_) + " full-text results";
        gfx::draw_text(c, Face::Sans, kSmallPx, kMarginX, kContentTop + 56, sub, t.muted);
        if (rows_.empty()) {
            if (busy_) draw_loading(c, area, "Searching\xE2\x80\xA6");
            else {
                const svc::ZimBook* b = find_book(content_);
                const std::string none = b && b->local ? "No article title starts with that. Collections on this PS4 or a USB "
                                                         "drive are searched by title only (full-text search needs a Kiwix Server)."
                                                       : "Try other words.";
                draw_message(c, area, "Nothing found", error_.empty() ? none : error_, !error_.empty());
            }
            return;
        }
        int y = kContentTop + 110;
        for (int i = nav_.top; i < std::min((int)rows_.size(), nav_.top + kVisible); ++i) {
            const Row& row = rows_[i];
            const bool on = i == nav_.index;
            if (row.header) {
                gfx::draw_text(c, Face::SansSemiBold, 23, kMarginX + 8, y + 30, row.title, t.muted);
                y += kRowH;
                continue;
            }
            const Rect r{kMarginX, y, kContentW, kRowH - 10};
            draw_panel(c, r, on);
            if (on) draw_focus(c, r, 14);
            gfx::draw_text(c, Face::SansSemiBold, 28, r.x + 28, r.y + 12, gfx::ellipsize(Face::SansSemiBold, 28, row.title, r.w - 260), row.more ? t.accent : t.text);
            if (!row.words.empty()) {
                const std::string w = row.words + " words";
                gfx::draw_text(c, Face::Sans, 21, r.right() - 28 - gfx::text_width(Face::Sans, 21, w), r.y + 18, w, t.faint);
            }
            if (!row.snippet.empty())
                gfx::draw_text(c, Face::Sans, 22, r.x + 28, r.y + 52, gfx::ellipsize(Face::Sans, 22, row.snippet, r.w - 56), t.muted);
            y += kRowH;
        }
        if (busy_) draw_spinner(c, kWidth - kMarginX - 20, kContentTop + 30, 16);
        draw_scrollbar(c, {kWidth - kMarginX + 30, kContentTop + 110, 8, kVisible * kRowH}, nav_.top, kVisible, (int)rows_.size());
    }

private:
    struct Row {
        std::string title, url, snippet, words;
        bool header = false, more = false;
    };
    static constexpr int kRowH = 92, kVisible = 8, kPage = 25;

    void run() {
        rows_.clear();
        titles_.clear();
        text_.clear();
        total_ = 0;
        error_.clear();
        nav_ = ListNav();
        const std::string srv = server_for(content_), content = content_, q = query_;
        ++busy_;
        struct Result {
            std::vector<svc::SearchHit> hits;
            std::string error;
            int total = 0;
        };
        lib::async<Result>(
            token_,
            [srv, content, q]() {
                Result r;
                svc::kiwix::suggest(srv, content, q, 10, r.hits, r.error);
                return r;
            },
            [this](Result& r) {
                --busy_;
                titles_ = std::move(r.hits);
                rebuild();
            });
        ++busy_;
        lib::async<Result>(
            token_,
            [srv, content, q]() {
                Result r;
                svc::kiwix::search(srv, content, q, 0, kPage, r.hits, r.total, r.error);
                return r;
            },
            [this](Result& r) {
                --busy_;
                text_ = std::move(r.hits);
                total_ = r.total;
                if (text_.empty() && titles_.empty()) error_ = r.error;
                // A file on the console or a USB drive has no full-text search:
                // finding no title is not an error there.
                if (const svc::ZimBook* b = find_book(content_))
                    if (b->local) error_.clear();
                rebuild();
            });
    }

    void more() {
        const std::string srv = server_for(content_), content = content_, q = query_;
        const int start = (int)text_.size();
        ++busy_;
        struct Result {
            std::vector<svc::SearchHit> hits;
            std::string error;
            int total = 0;
        };
        lib::async<Result>(
            token_,
            [srv, content, q, start]() {
                Result r;
                svc::kiwix::search(srv, content, q, start, kPage, r.hits, r.total, r.error);
                return r;
            },
            [this](Result& r) {
                --busy_;
                for (auto& h : r.hits) text_.push_back(std::move(h));
                rebuild();
            });
    }

    void rebuild() {
        const int keep = nav_.index;
        rows_.clear();
        if (!titles_.empty()) {
            rows_.push_back({"Matching titles", "", "", "", true});
            for (const auto& h : titles_) rows_.push_back({h.title, h.url, "", ""});
        }
        if (!text_.empty()) {
            rows_.push_back({"Full-text results", "", "", "", true});
            for (const auto& h : text_) rows_.push_back({h.title, h.url, h.snippet, h.words});
            if ((int)text_.size() < total_) rows_.push_back({"More results\xE2\x80\xA6", "", "", "", false, true});
        }
        nav_.index = keep;
        if (!rows_.empty() && rows_[std::min(keep, (int)rows_.size() - 1)].header) nav_.index = keep + 1;
        nav_.clamp((int)rows_.size(), kVisible);
    }

    std::string content_, title_, query_, error_;
    std::vector<svc::SearchHit> titles_, text_;
    std::vector<Row> rows_;
    int total_ = 0, busy_ = 0;
    ListNav nav_;
};

}  // namespace

std::unique_ptr<Screen> make_knowledge_screen() {
    // No server, but the online library chosen: it is the tab.
    if (server().empty() && svc::settings().kiwix_online && svc::zim_shelf().count() == 0) return make_kiwix_online_root();
    return std::make_unique<KnowledgeScreen>();
}

std::string kiwix_language_name(const std::string& code) { return language_name(code); }

void draw_kiwix_icon(gfx::Canvas& c, const gfx::Rect& r, const svc::ZimBook& book) { draw_collection_icon(c, r, book); }

void open_kiwix_collection(const svc::ZimBook& book) { app().push(std::make_unique<CollectionScreen>(book)); }

void open_zim_file(const std::string& path) {
    struct Result {
        svc::ZimBook book;
        std::string error;
        bool ok = false;
    };
    app().toast("Opening the collectionâ¦");
    lib::async<Result>(
        lib::make_token(),
        [path]() {
            Result r;
            r.ok = svc::zim_shelf().adopt(path, r.book, r.error);
            return r;
        },
        [](Result& r) {
            if (!r.ok) {
                app().toast(r.error);
                return;
            }
            catalog_cache().local = svc::zim_shelf().books();
            catalog_cache().local_generation = svc::zim_shelf().generation();
            open_kiwix_collection(r.book);
        });
}

void open_kiwix_search(const std::string& content, const std::string& title) {
    auto screen = std::make_unique<SearchScreen>(content, title);
    SearchScreen* raw = screen.get();
    app().push(std::move(screen));
    raw->ask();
}

void open_article(const std::string& url, const std::string& collection_title) {
    open_reader(std::make_unique<KiwixSource>(url, collection_title));
}

}  // namespace ui
