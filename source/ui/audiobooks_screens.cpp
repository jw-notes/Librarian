// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The Audiobooks tab: Audiobookshelf's libraries and downloaded audiobooks, a
// book's page (play, chapters, download, finished), and the player. Playback
// itself belongs to svc::listening(), so it carries on when these screens close.
#include <math.h>
#include <string.h>

#include <algorithm>
#include <functional>

#include "audio/player.h"
#include "core/text.h"
#include "gfx/font.h"
#include "gfx/image_cache.h"
#include "services/audiobookshelf.h"
#include "services/librivox.h"
#include "doc/covers.h"
#include "services/library.h"
#include "services/listening.h"
#include "services/local_library.h"
#include "services/local_files.h"
#include "services/settings.h"
#include "ui/app.h"
#include "ui/openers.h"
#include "ui/overlays.h"
#include "ui/reader.h"
#include "ui/troubleshoot.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;
namespace abs = svc::abs;

void open_audiobook(const abs::Book& book);

namespace {

svc::Listening& L() { return svc::listening(); }

// Talking books (local documents) in the grid: "talking:<path>".
const char* const kTalkingPrefix = "talking:";

std::string cover_of(const abs::Book& b) {
    if (const svc::AudiobookDownload* d = L().download_of(b.id))
        if (!d->cover.empty()) return d->cover;
    if (!b.cover_url.empty()) return b.cover_url;
    // Only Audiobookshelf's own books have covers there; the others (LibriVox
    // books not on archive.org yet, folders without a picture) are looked up.
    if (svc::is_local_book(b.id)) return doc::find_cover_key(b.title, b.author);
    return abs::cover_source(b.id);
}

// Nothing to show: no server, no LibriVox, no downloads.
bool nothing() { return !abs::configured() && !svc::settings().librivox && L().downloads().empty(); }

abs::Status librivox_status(bool ok, const std::string& error) {
    abs::Status st;
    if (!ok) st.error = error.empty() ? "LibriVox did not answer." : error;
    return st;
}

// 0..1, from what the console knows (merged with the server's progress).
float progress_of(const abs::Book& b) {
    const svc::LocalProgress p = L().progress(b.id);
    if (p.updated) return p.finished ? 1.f : p.duration > 0 ? (float)(p.time / p.duration) : 0.f;
    if (b.progress.found) return b.progress.finished ? 1.f : (float)b.progress.progress;
    return 0.f;
}

std::string speed_text(double s) {
    char out[16];
    snprintf(out, sizeof out, s == floor(s) ? "%.0f\xC3\x97" : fmod(s * 10, 1) == 0 ? "%.1f\xC3\x97" : "%.2f\xC3\x97", s);
    return out;
}

// A "play" triangle pointing right, `h` tall, from (x, y).
void play_glyph(gfx::Canvas& c, int x, int y, int h, gfx::Color color) {
    const int w = h * 5 / 6;
    for (int i = 0; i < h; ++i) {
        const float d = fabsf(i + 0.5f - h / 2.f) / (h / 2.f);
        c.fill({x, y + i, std::max(1, (int)(w * (1 - d))), 1}, color);
    }
}

void draw_check(gfx::Canvas& c, int cx, int cy) {
    c.fill_circle(cx, cy, 14, theme().success);
    c.line(cx - 7, cy, cx - 1, cy + 6, 3, gfx::rgb(0xFFFFFF));
    c.line(cx - 1, cy + 6, cx + 8, cy - 6, 3, gfx::rgb(0xFFFFFF));
}

// ---- a grid of square covers --------------------------------------------------------------

class BookGrid {
public:
    std::vector<abs::Book> books;
    std::function<void()> near_end;  // to load the next page

    const abs::Book* focused() const { return nav_.index < (int)books.size() ? &books[nav_.index] : nullptr; }
    void reset() { nav_ = GridNav(); }

    bool update(const Input& in, bool allow_left_exit) {
        const int n = (int)books.size();
        if (!n) return false;
        if (allow_left_exit && in.repeat(kLeft) && nav_.index % cols_ == 0) return false;
        const bool moved = nav_.update(in, n, cols_, rows_);
        if (near_end && nav_.index >= n - cols_ * 2) near_end();
        if (in.press(kCross)) open_audiobook(books[nav_.index]);
        return moved;
    }

    void draw(gfx::Canvas& c, const Rect& area, bool focused_view) {
        const Theme& t = theme();
        const int gap = 30;
        cols_ = std::max(2, (area.w + gap) / (230 + gap));
        const int card = (area.w - (cols_ - 1) * gap) / cols_, row_h = card + 92;
        rows_ = std::max(1, (area.h + 20) / row_h);
        nav_.clamp((int)books.size(), cols_, rows_);
        for (int i = nav_.top_row * cols_; i < std::min((int)books.size(), (nav_.top_row + rows_) * cols_); ++i) {
            const abs::Book& b = books[i];
            const int row = i / cols_ - nav_.top_row, col = i % cols_;
            const Rect cover{area.x + col * (card + gap), area.y + row * row_h, card, card};
            const bool on = focused_view && i == nav_.index;
            draw_cover(c, cover, cover_of(b), b.title, b.author);
            if (L().download_of(b.id)) draw_check(c, cover.right() - 22, cover.y + 22);
            if (L().active() && L().now().id == b.id) {
                c.fill_rounded({cover.x + 10, cover.y + 10, 44, 44}, 22, t.accent);
                play_glyph(c, cover.x + 27, cover.y + 20, 24, t.accent_text);
            }
            const float p = progress_of(b);
            if (p > 0) draw_progress(c, {cover.x, cover.bottom() - 6, cover.w, 6}, p);
            if (on) draw_focus(c, cover, 10);
            gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 22, cover.x, cover.bottom() + 12,
                           gfx::ellipsize(Face::SansSemiBold, 22, b.title, cover.w), on ? t.text : t.muted);
            gfx::draw_text(c, Face::Sans, 19, cover.x, cover.bottom() + 44, gfx::ellipsize(Face::Sans, 19, b.author, cover.w), t.faint);
        }
        const int total_rows = ((int)books.size() + cols_ - 1) / cols_;
        draw_scrollbar(c, {area.right() + 24, area.y, 8, area.h}, nav_.top_row, rows_, total_rows);
    }

private:
    GridNav nav_;
    int cols_ = 6, rows_ = 2;
};

// ---- the Audiobooks tab ----------------------------------------------------------------------

class AudiobooksScreen : public Screen {
public:
    AudiobooksScreen() {
        L().refresh_downloads();
        if (abs::configured()) {
            L().sync_with_server();  // progress and bookmarks, and anything waiting to be sent
            load_libraries();
        }
        build_sections();
    }

    Tab tab() const override { return Tab::Audiobooks; }
    bool animating() const override { return loading_ || libraries_loading_; }
    void resumed() override { build_sections(); }

    std::vector<Hint> hints() const override {
        if (nothing()) return {{kSquare, "LibriVox (free)"}, {kCross, "Set up Audiobookshelf"}};
        std::vector<Hint> h = {{kTriangle, "Options"}};
        if (on_librivox()) h.insert(h.begin(), {kSquare, "Search LibriVox"});
        else if (abs::configured()) h.insert(h.begin(), {kSquare, error_.empty() ? "Search" : "Troubleshoot"});
        if (L().active()) h.push_back({kTouchPad, "Player"});
        h.insert(h.begin(), {kCross, "Open"});
        return h;
    }

    void update(const Input& in) override {
        if (nothing()) {
            if (in.press(kCross)) app().switch_tab(Tab::Settings);
            if (in.press(kSquare)) {
                svc::settings().librivox = true;
                svc::settings_save();
                build_sections();
            }
            return;
        }
        if (generation_ != L().generation() || local_generation_ != svc::local_library().generation()) build_sections();
        if (in.press(kTriangle)) {
            options();
            return;
        }
        if (in.press(kSquare) && on_librivox()) {
            search_librivox();
            return;
        }
        if (in.press(kSquare) && abs::configured()) {
            if (!error_.empty()) open_troubleshooter(Service::Audiobookshelf, status_, error_);
            else search();
            return;
        }
        if (in_content_) {
            if (!grid_.update(in, true) && in.repeat(kLeft)) in_content_ = false;
            return;
        }
        const int before = nav_.index;
        nav_.update(in, (int)sections_.size(), kSideRows, false);
        if (nav_.index != before) select();
        const Section* s = section();
        if (s && s->kind == Kind::NowPlaying && (in.press(kCross) || in.press(kRight))) {
            open_player();
            return;
        }
        if (s && s->kind == Kind::Retry && in.press(kCross)) {
            load_libraries();
            return;
        }
        if ((in.repeat(kRight) || in.press(kCross)) && !grid_.books.empty()) in_content_ = true;
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        const Rect area{kMarginX, kContentTop, kContentW, kFooterTop - kContentTop};
        if (nothing()) {
            draw_message(c, area, "Audiobooks",
                         "Listen to free public-domain audiobooks from LibriVox (press Square), or to the audiobooks on "
                         "your Audiobookshelf server (press X to add it in Settings). Books can be downloaded to the "
                         "console or a USB drive for listening offline.");
            return;
        }
        const int side_w = 380;
        int y = kContentTop;
        nav_.clamp((int)sections_.size(), kSideRows);
        for (int i = nav_.top; i < std::min((int)sections_.size(), nav_.top + kSideRows); ++i) {
            const bool on = i == nav_.index;
            const Rect r{kMarginX, y, side_w, 62};
            if (on) c.fill_rounded(r, 12, in_content_ ? t.surface_hi : t.accent);
            gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 26, r.x + 22, r.y + 13,
                           gfx::ellipsize(Face::Sans, 26, sections_[i].title, side_w - 44),
                           on && !in_content_ ? t.accent_text : on ? t.text : t.muted);
            y += 68;
        }
        if (libraries_loading_) draw_spinner(c, kMarginX + side_w - 30, y + 30, 12);
        const Rect content{kMarginX + side_w + 50, kContentTop, kContentW - side_w - 90, kFooterTop - kContentTop - 10};
        const Section* s = section();
        if (!s) return;
        if (s->kind == Kind::NowPlaying) {
            draw_message(c, content, L().now().title, "Now playing. Press X to open the player.");
            return;
        }
        if (s->kind == Kind::Retry) {
            const Diagnosis d = diagnose(Service::Audiobookshelf, status_, error_);
            draw_message(c, content, d.title.empty() ? "Could not reach Audiobookshelf" : d.title,
                         error_ + " Press Square for troubleshooting steps, or X to try again." +
                             (L().downloads().empty() ? "" : " Downloaded audiobooks can still be played."),
                         true);
            return;
        }
        if (grid_.books.empty()) {
            if (loading_) draw_loading(c, content, "Loading\xE2\x80\xA6");
            else if (!content_error_.empty()) draw_message(c, content, "Could not load this list", content_error_, true);
            else if (s->kind == Kind::Downloaded)
                draw_message(c, content, "No downloads yet",
                             "Open a book and choose Download to listen without the network. Settings › System decides "
                             "whether downloads go to the console or a USB drive.");
            else if (s->kind == Kind::Continue)
                draw_message(c, content, "Nothing in progress", "Books you have started listening to appear here.");
            else if (s->kind == Kind::Librivox || s->kind == Kind::LibrivoxSearch)
                draw_message(c, content, "Nothing found",
                             s->kind == Kind::Librivox ? "LibriVox lists no recent books. Press Square to search it."
                                                       : "No LibriVox book has a title or author starting with that.");
            else draw_message(c, content, "Nothing here", "This library has no audiobooks.");
            return;
        }
        grid_.draw(c, content, in_content_);
        if (loading_) draw_spinner(c, content.right() - 20, content.y - 30, 14);
    }

private:
    enum class Kind { NowPlaying, Continue, Library, Downloaded, Retry, Librivox, LibrivoxSearch, Talking };
    struct Section {
        Kind kind;
        std::string title, library;
    };
    static constexpr int kSideRows = 12, kPage = 60;

    const Section* section() const { return nav_.index < (int)sections_.size() ? &sections_[nav_.index] : nullptr; }
    bool on_librivox() const {
        const Section* s = section();
        return (s && (s->kind == Kind::Librivox || s->kind == Kind::LibrivoxSearch)) ||
               (!abs::configured() && svc::settings().librivox);
    }

    void search_librivox() {
        open_keyboard("Search LibriVox (title or author)", lv_query_, KeyboardKind::Text, [this](const std::string& v) {
            lv_query_ = lib::trim(v);
            if (lv_query_.empty()) return;
            build_sections();
            for (int i = 0; i < (int)sections_.size(); ++i)
                if (sections_[i].kind == Kind::LibrivoxSearch) nav_.index = i;
            select();
        });
    }

    void build_sections() {
        generation_ = L().generation();
        local_generation_ = svc::local_library().generation();
        const Section* old = section();
        const std::string keep = old ? old->title : std::string();
        std::vector<Section> s;
        if (L().active()) s.push_back({Kind::NowPlaying, "Now Playing", ""});
        if (abs::configured()) {
            if (!error_.empty() && libraries_.empty()) s.push_back({Kind::Retry, "Not connected", ""});
            else s.push_back({Kind::Continue, "Continue Listening", ""});
            for (const auto& l : libraries_)
                if (l.media_type == "book") s.push_back({Kind::Library, l.name, l.id});
        }
        if (svc::settings().librivox) {
            s.push_back({Kind::Librivox, "LibriVox: new", ""});
            if (!lv_query_.empty()) s.push_back({Kind::LibrivoxSearch, "LibriVox: \xE2\x80\x9C" + lv_query_ + "\xE2\x80\x9D", lv_query_});
        }
        s.push_back({Kind::Downloaded, "Downloaded", ""});
        if (!svc::local_library().talking_books().empty()) s.push_back({Kind::Talking, "Talking books", ""});
        const bool same = s.size() == sections_.size() &&
                          std::equal(s.begin(), s.end(), sections_.begin(),
                                     [](const Section& a, const Section& b) { return a.title == b.title && a.kind == b.kind; });
        sections_ = std::move(s);
        if (same) {
            // Downloads and progress may have changed: refresh what shows.
            if (section() && (section()->kind == Kind::Downloaded || section()->kind == Kind::Talking)) select(false);
            return;
        }
        nav_.index = 0;
        for (int i = 0; i < (int)sections_.size(); ++i)
            if (sections_[i].title == keep) nav_.index = i;
        // Open on the first useful list: Continue Listening, else the library.
        if (keep.empty() && sections_[nav_.index].kind == Kind::NowPlaying && sections_.size() > 1) nav_.index = 1;
        select();
    }

    void load_libraries() {
        libraries_loading_ = true;
        error_.clear();
        struct Result {
            abs::Status st;
            std::vector<abs::Library> libs;
        };
        lib::async<Result>(
            token_,
            [] {
                Result r;
                r.st = abs::libraries(r.libs);
                return r;
            },
            [this](Result& r) {
                libraries_loading_ = false;
                status_ = r.st.status;
                error_ = r.st.error;
                libraries_ = r.libs;
                build_sections();
            });
    }

    void select(bool reset = true) {
        const Section* s = section();
        if (reset) {
            grid_.books.clear();
            grid_.reset();
            in_content_ = false;
        }
        content_error_.clear();
        grid_.near_end = nullptr;
        if (!s) return;
        ++request_;
        switch (s->kind) {
            case Kind::Downloaded: {
                std::vector<abs::Book> books;
                for (const auto& d : L().downloads()) books.push_back(d.book);
                grid_.books = std::move(books);
                loading_ = false;
                return;
            }
            case Kind::Talking: {
                // Read-along EPUBs and DAISY books on this PS4 and its drives:
                // opened in the reader, reading aloud.
                std::vector<abs::Book> books;
                for (const svc::LocalDoc* d : svc::local_library().talking_books()) {
                    abs::Book b;
                    b.id = kTalkingPrefix + d->path;
                    const svc::Item* it = svc::library().find_file(d->path, d->fingerprint);
                    b.title = it && !it->title.empty() ? it->title : d->title;
                    b.author = it && !it->author.empty() ? it->author : d->author;
                    b.cover_url = it && !it->cover.empty() ? it->cover : doc::cover_key(d->path);
                    books.push_back(std::move(b));
                }
                grid_.books = std::move(books);
                loading_ = false;
                return;
            }
            case Kind::Continue: load_list([](std::vector<abs::Book>& out, int&) { return abs::in_progress(out); }); return;
            case Kind::Librivox:
                load_list([](std::vector<abs::Book>& out, int&) {
                    std::string error;
                    return librivox_status(svc::librivox::recent(out, error), error);
                });
                return;
            case Kind::LibrivoxSearch: {
                const std::string q = s->library;
                load_list([q](std::vector<abs::Book>& out, int&) {
                    std::string error;
                    return librivox_status(svc::librivox::search(q, out, error), error);
                });
                return;
            }
            case Kind::Library: {
                page_ = 0;
                total_ = -1;
                svc::settings().abs_library = s->library;
                load_page();
                return;
            }
            default:
                loading_ = false;
                return;
        }
    }

    void load_list(std::function<abs::Status(std::vector<abs::Book>&, int&)> fetch) {
        loading_ = true;
        const int request = request_;
        struct Result {
            abs::Status st;
            std::vector<abs::Book> books;
            int total = 0;
        };
        lib::async<Result>(
            token_,
            [fetch] {
                Result r;
                r.st = fetch(r.books, r.total);
                return r;
            },
            [this, request](Result& r) {
                if (request != request_) return;
                loading_ = false;
                if (!r.st.ok()) {
                    content_error_ = r.st.error;
                    return;
                }
                grid_.books = std::move(r.books);
            });
    }

    void load_page() {
        const Section* s = section();
        if (!s || s->kind != Kind::Library || loading_) return;
        if (total_ >= 0 && (int)grid_.books.size() >= total_) return;
        loading_ = true;
        const int request = request_, page = page_;
        const std::string library = s->library, sort = sort_;
        struct Result {
            abs::Status st;
            std::vector<abs::Book> books;
            int total = 0;
        };
        lib::async<Result>(
            token_,
            [library, page, sort] {
                Result r;
                r.st = abs::books(library, page, kPage, sort, r.books, r.total);
                return r;
            },
            [this, request](Result& r) {
                if (request != request_) return;
                loading_ = false;
                if (!r.st.ok()) {
                    if (grid_.books.empty()) content_error_ = r.st.error;
                    return;
                }
                total_ = r.total;
                ++page_;
                for (auto& b : r.books) grid_.books.push_back(std::move(b));
                grid_.near_end = [this] { load_page(); };
            });
    }

    void options() {
        std::vector<MenuItem> items;
        const Section* s = section();
        if (s && s->kind == Kind::Library) {
            items.push_back({"Sort by",
                             [this] {
                                 return std::string(sort_ == "author" ? "Author" : sort_ == "added" ? "Recently added" : "Title");
                             },
                             nullptr, [this](int d) {
                                 static const char* sorts[] = {"title", "author", "added"};
                                 int i = 0;
                                 for (int k = 0; k < 3; ++k)
                                     if (sort_ == sorts[k]) i = k;
                                 sort_ = sorts[(i + 3 + d) % 3];
                                 select();
                             }});
        }
        items.push_back({"Refresh", nullptr, [this] {
                             L().refresh_downloads();
                             if (abs::configured()) {
                                 L().sync_with_server();
                                 load_libraries();
                             }
                             select();
                         },
                         nullptr});
        if (L().active()) {
            items.push_back({"Open the player", nullptr, [] { open_player(); }, nullptr});
            MenuItem stop{"Stop playback", nullptr, [] {
                              L().stop();
                              app().toast("Stopped");
                          },
                          nullptr};
            stop.destructive = true;
            items.push_back(stop);
        }
        open_menu("Audiobooks", std::move(items));
    }

    void search();

    std::vector<Section> sections_;
    std::vector<abs::Library> libraries_;
    ListNav nav_;
    BookGrid grid_;
    bool in_content_ = false, loading_ = false, libraries_loading_ = false;
    std::string error_, content_error_, sort_ = "title", lv_query_;
    int status_ = 0, page_ = 0, total_ = -1, request_ = 0;
    uint64_t generation_ = 0, local_generation_ = 0;
};

// ---- search results ------------------------------------------------------------------------

class SearchScreen : public Screen {
public:
    explicit SearchScreen(std::string query) : query_(std::move(query)) {
        loading_ = true;
        struct Result {
            abs::Status st;
            std::vector<abs::Book> books;
        };
        const std::string q = query_;
        lib::async<Result>(
            token_,
            [q] {
                // Every audiobook library.
                Result r;
                std::vector<abs::Library> libs;
                r.st = abs::libraries(libs);
                for (const auto& l : libs) {
                    if (l.media_type != "book") continue;
                    std::vector<abs::Book> found;
                    r.st = abs::search(l.id, q, found);
                    if (!r.st.ok()) break;
                    for (auto& b : found) r.books.push_back(std::move(b));
                }
                return r;
            },
            [this](Result& r) {
                loading_ = false;
                error_ = r.st.error;
                grid_.books = std::move(r.books);
            });
    }

    Tab tab() const override { return Tab::Audiobooks; }
    bool animating() const override { return loading_; }
    std::vector<Hint> hints() const override { return {{kCircle, "Back"}, {kCross, "Open"}}; }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        grid_.update(in, false);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        gfx::draw_text(c, Face::SansBold, 40, kMarginX, kContentTop,
                       gfx::ellipsize(Face::SansBold, 40, "\xE2\x80\x9C" + query_ + "\xE2\x80\x9D", kContentW), t.text);
        const Rect area{kMarginX, kContentTop + 76, kContentW - 40, kFooterTop - kContentTop - 86};
        if (grid_.books.empty()) {
            if (loading_) draw_loading(c, area, "Searching\xE2\x80\xA6");
            else if (!error_.empty()) draw_message(c, area, "Could not search", error_, true);
            else draw_message(c, area, "Nothing found", "No audiobook's title, author, narrator or series matches.");
            return;
        }
        grid_.draw(c, area, true);
    }

private:
    std::string query_, error_;
    BookGrid grid_;
    bool loading_ = false;
};

void AudiobooksScreen::search() {
    open_keyboard("Search audiobooks", "", KeyboardKind::Text, [](const std::string& q) {
        if (!lib::trim(q).empty()) app().push(std::make_unique<SearchScreen>(lib::trim(q)));
    });
}

// ---- a book ----------------------------------------------------------------------------------

class AudiobookScreen : public Screen {
public:
    explicit AudiobookScreen(abs::Book book) : b_(std::move(book)) {
        // A downloaded copy knows its chapters and files; the server may know more.
        if (const svc::AudiobookDownload* d = L().download_of(b_.id))
            if (b_.files.empty()) {
                const abs::Progress keep = b_.progress;
                b_ = d->book;
                b_.progress = keep;
            }
        // The listener's own folders are not on the server; LibriVox's books
        // come from LibriVox.
        if (svc::librivox::is_librivox(b_.id)) {
            if (b_.files.empty()) load();
        } else if (abs::configured() && !svc::is_local_book(b_.id)) {
            load();
        }
        build();
    }

    Tab tab() const override { return Tab::Audiobooks; }
    bool animating() const override { return downloading_ || loading_; }
    void resumed() override { build(); }
    std::vector<Hint> hints() const override {
        if (downloading_) return {{kCircle, "Cancel download"}};
        std::vector<Hint> h = {{kCircle, "Back"}, {kCross, "Select"}};
        if (L().active()) h.push_back({kTouchPad, "Player"});
        return h;
    }

    void update(const Input& in) override {
        if (generation_ != L().generation()) build();
        if (in.press(kCircle)) {
            if (downloading_) *cancel_ = true;
            else app().pop();
            return;
        }
        if (downloading_) return;
        if (in.repeat(kUp) && action_ > 0) --action_;
        if (in.repeat(kDown) && action_ + 1 < (int)actions_.size()) ++action_;
        if (in.repeat(kRight)) text_top_ += 3;
        if (in.repeat(kLeft)) text_top_ = std::max(0, text_top_ - 3);
        if (in.press(kCross) && action_ < (int)actions_.size()) actions_[action_].run();
    }

    void draw(gfx::Canvas& c) override {
        if (progress_) {
            done_ = progress_->first;
            total_ = progress_->second;
        }
        const Theme& t = theme();
        const Rect cover{kMarginX, kContentTop + 10, 520, 520};
        draw_cover(c, cover, cover_of(b_), b_.title, b_.author);
        const float p = progress_of(b_);
        if (p > 0) {
            draw_progress(c, {cover.x, cover.bottom() + 20, cover.w, 8}, p);
            const svc::LocalProgress lp = L().progress(b_.id);
            const double left = b_.duration - (lp.updated ? lp.time : b_.progress.current_time);
            const std::string label = p >= 1 ? std::string("Finished") : svc::format_duration(left) + " left";
            gfx::draw_text(c, Face::Sans, 23, cover.x, cover.bottom() + 42, label, t.muted);
        }
        const int x = cover.right() + 60, w = kWidth - kMarginX - x;
        int y = kContentTop + 4;
        y += gfx::draw_wrapped(c, Face::SerifBold, 50, x, y, w, b_.title, t.text, 2, 64) + 4;
        if (!b_.subtitle.empty()) {
            gfx::draw_text(c, Face::Serif, 28, x, y, gfx::ellipsize(Face::Serif, 28, b_.subtitle, w), t.muted);
            y += 46;
        }
        if (!b_.author.empty()) {
            gfx::draw_text(c, Face::Sans, 30, x, y, gfx::ellipsize(Face::Sans, 30, b_.author, w), t.accent);
            y += 48;
        }
        std::string facts;
        auto add = [&](const std::string& s) {
            if (!s.empty()) facts += (facts.empty() ? "" : "   \xC2\xB7   ") + s;
        };
        if (!b_.narrator.empty()) add("Narrated by " + b_.narrator);
        add(b_.series);
        add(b_.year);
        if (b_.duration > 0) add(svc::format_duration(b_.duration));
        const int chapters = b_.chapters.empty() ? b_.chapter_count : (int)b_.chapters.size();
        if (chapters) add(std::to_string(chapters) + (chapters == 1 ? " chapter" : " chapters"));
        if (const svc::AudiobookDownload* d = L().download_of(b_.id)) {
            if (d->local) add(std::string(d->on_usb ? "Your folder on USB" : "Your folder on the console") + " (" + lib::format_bytes(d->bytes) + ")");
            else add(std::string(d->on_usb ? "Downloaded to USB" : "Downloaded") + " (" + lib::format_bytes(d->bytes) + ")");
        }
        if (!facts.empty()) {
            y += gfx::draw_wrapped(c, Face::Sans, 23, x, y, w, facts, t.muted, 2, 34) + 16;
        }
        const int bw = 440;
        int by = y + 10;
        for (int i = 0; i < (int)actions_.size(); ++i) {
            const Rect r{x, by, bw, 70};
            const bool on = i == action_ && !downloading_;
            c.fill_rounded(r, 14, on ? t.accent : t.surface);
            gfx::draw_text(c, Face::SansSemiBold, 26, r.x + 26, r.y + 18, gfx::ellipsize(Face::SansSemiBold, 26, actions_[i].label, bw - 52),
                           on ? t.accent_text : t.text);
            by += 82;
        }
        if (downloading_) {
            draw_progress(c, {x, by + 10, bw, 12}, total_ ? (float)done_ / total_ : 0.f);
            gfx::draw_text(c, Face::Sans, 22, x, by + 34,
                           "Downloading " + lib::format_bytes(done_) + (total_ ? " of " + lib::format_bytes(total_) : ""), t.muted);
        }
        if (loading_) draw_spinner(c, x + bw + 30, y + 45, 14);
        if (!b_.description.empty()) {
            const int sx = x + bw + 50, sw = x + w - sx;
            const auto lines = gfx::wrap_text(Face::Serif, 25, b_.description, sw);
            const int line_h = 38, max_lines = (kFooterTop - 30 - (y + 10)) / line_h;
            text_top_ = std::min(text_top_, std::max(0, (int)lines.size() - max_lines));
            for (int i = text_top_; i < (int)lines.size() && i < text_top_ + max_lines; ++i)
                gfx::draw_text(c, Face::Serif, 25, sx, y + 10 + (i - text_top_) * line_h, lines[i], t.muted);
        }
    }

private:
    struct Action {
        std::string label;
        std::function<void()> run;
    };

    void load() {
        loading_ = true;
        const std::string id = b_.id;
        lib::async<std::pair<abs::Status, abs::Book>>(
            token_,
            [id] {
                std::pair<abs::Status, abs::Book> r;
                if (svc::librivox::is_librivox(id)) {
                    std::string error;
                    r.first = librivox_status(svc::librivox::book(id, r.second, error), error);
                } else {
                    r.first = abs::item(id, r.second);
                }
                return r;
            },
            [this](std::pair<abs::Status, abs::Book>& r) {
                loading_ = false;
                if (r.first.ok()) {
                    b_ = std::move(r.second);
                    build();
                }
            });
    }

    void build() {
        generation_ = L().generation();
        actions_.clear();
        const bool playing = L().active() && L().now().id == b_.id;
        const svc::LocalProgress lp = L().progress(b_.id);
        const double resume = L().resume_position(b_.id, b_.progress);
        const bool finished = lp.updated ? lp.finished : b_.progress.finished;
        const svc::AudiobookDownload* copy = L().download_of(b_.id);
        if (playing) actions_.push_back({"Open the player", [] { open_player(); }});
        else if (resume > 1 && !finished)
            actions_.push_back({"Resume from " + svc::format_duration(resume), [this] { play(-1); }});
        else actions_.push_back({"Play", [this] { play(0); }});
        if (!playing && (resume > 1 || finished)) actions_.push_back({"Play from the beginning", [this] { play(0); }});
        if (!b_.chapters.empty()) actions_.push_back({"Chapters", [this] { chapters(); }});
        if (copy && copy->local) {
            // The listener's own files: never deleted from here.
        } else if (copy) {
            const svc::AudiobookDownload d = *copy;
            actions_.push_back({"Delete download", [d] {
                                    confirm("Delete the downloaded copy?",
                                            svc::librivox::is_librivox(d.item_id) ? "It can be downloaded again from LibriVox."
                                                                                  : "It stays on your Audiobookshelf server.",
                                            "Delete", [d] {
                                        if (L().active() && L().now().id == d.item_id && L().now().local) L().stop();
                                        svc::delete_audiobook_download(d);
                                        L().refresh_downloads();
                                        app().toast("Download deleted");
                                    });
                                }});
        } else if ((abs::configured() || svc::librivox::is_librivox(b_.id)) && !b_.files.empty()) {
            std::string why;
            if (svc::audiobook_offline_playable(b_, why)) {
                uint64_t size = 0;
                for (const auto& f : b_.files) size += f.size;
                const std::string where = svc::settings().download_to_usb && !svc::first_connected_usb().empty() ? "to USB" : "";
                // LibriVox lists no sizes: its length instead.
                const std::string amount = size ? lib::format_bytes(size) : svc::format_duration(b_.duration);
                actions_.push_back({"Download " + where + (where.empty() ? "" : " ") + "(" + amount + ")",
                                    [this] { download(); }});
            } else {
                actions_.push_back({"Download", [why] { app().toast(why); }});
            }
        }
        actions_.push_back({finished ? "Mark as not finished" : "Mark as finished", [this, finished] {
                                L().mark_finished(b_.id, b_.duration, !finished);
                                build();
                            }});
        action_ = std::min(action_, (int)actions_.size() - 1);
    }

    void play(double at) {
        L().play(b_, at);
        if (!L().problem().empty()) {
            app().toast(L().problem());
            return;
        }
        open_player();
    }

    void chapters() {
        std::vector<MenuItem> items;
        for (const auto& ch : b_.chapters) {
            const double start = ch.start;
            items.push_back({ch.title, [start] { return svc::format_duration(start); }, [this, start] { play(start); }, nullptr});
        }
        open_menu(b_.title, std::move(items), std::to_string(b_.chapters.size()) + " chapters");
    }

    void download() {
        downloading_ = true;
        done_ = total_ = 0;
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        auto cancel = cancel_;
        auto progress = std::make_shared<std::pair<std::atomic<uint64_t>, std::atomic<uint64_t>>>();
        progress_ = progress;
        const abs::Book book = b_;
        struct Result {
            bool ok = false;
            std::string error;
        };
        // Not tied to this screen: a download finishes even if the page is closed.
        lib::async<Result>(
            nullptr,
            [book, cancel, progress] {
                Result r;
                svc::AudiobookDownload d;
                r.ok = svc::download_audiobook(book,
                                               [cancel, progress](uint64_t done, uint64_t total) {
                                                   progress->first = done;
                                                   progress->second = total;
                                                   platform::request_redraw();
                                                   return !*cancel;
                                               },
                                               d, r.error);
                return r;
            },
            [alive = token_, this](Result& r) {
                L().refresh_downloads();
                app().toast(r.ok ? "Downloaded: it plays without the network now" : r.error);
                if (*alive) return;  // the page was closed
                downloading_ = false;
            });
    }

    abs::Book b_;
    std::vector<Action> actions_;
    int action_ = 0, text_top_ = 0;
    bool downloading_ = false, loading_ = false;
    uint64_t done_ = 0, total_ = 0, generation_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::pair<std::atomic<uint64_t>, std::atomic<uint64_t>>> progress_;
};

// ---- the player ------------------------------------------------------------------------------

class PlayerScreen : public Screen {
public:
    bool full_screen() const override { return true; }
    Tab tab() const override { return Tab::Audiobooks; }
    bool animating() const override {
        const audio::State s = audio::player().state();
        return L().starting() || s == audio::State::Loading || s == audio::State::Buffering;
    }

    std::vector<Hint> hints() const override {
        return {{kCircle, "Back"}, {kOptions, "Stop"}, {kTriangle, "Menu"}, {kSquare, "Bookmark"}, {kL2, "Speed"}, {kL1, "Chapter"},
                {kLeft, "\xE2\x88\x92" "15 s / +30 s"}, {kCross, audio::player().state() == audio::State::Playing ? "Pause" : "Play"}};
    }

    void update(const Input& in) override {
        if (!L().active() && !L().starting()) {
            app().pop();
            return;
        }
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (L().starting()) return;
        if (in.press(kCross)) L().toggle();
        if (in.repeat(kLeft)) L().skip(-15);
        if (in.repeat(kRight)) L().skip(30);
        if (in.press(kL1)) L().previous_chapter();
        if (in.press(kR1)) L().next_chapter();
        if (in.repeat(kL2)) L().set_speed(audio::player().speed() - 0.1);
        if (in.repeat(kR2)) L().set_speed(audio::player().speed() + 0.1);
        if (in.press(kSquare)) bookmark();
        if (in.press(kTriangle)) menu();
        // Stop: where it stopped is saved and synced, and the player closes.
        if (in.press(kOptions)) {
            L().stop();
            app().pop();
            app().toast("Stopped");
        }
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        draw_background(c);
        const svc::NowPlaying& now = L().now();
        audio::Player& p = audio::player();
        const Rect cover{kMarginX + 20, 150, 680, 680};
        draw_cover(c, cover, now.cover, now.title, now.author);
        const int x = cover.right() + 90, w = kWidth - kMarginX - x;
        int y = 160;
        gfx::draw_text(c, Face::SansSemiBold, 23, x, y, now.local ? "NOW PLAYING  \xC2\xB7  DOWNLOADED" : "NOW PLAYING", t.accent);
        y += 50;
        y += gfx::draw_wrapped(c, Face::SerifBold, 54, x, y, w, now.title, t.text, 2, 68) + 8;
        if (!now.author.empty()) {
            gfx::draw_text(c, Face::Sans, 32, x, y, gfx::ellipsize(Face::Sans, 32, now.author, w), t.accent);
            y += 50;
        }
        if (!now.narrator.empty()) {
            gfx::draw_text(c, Face::Sans, 24, x, y, gfx::ellipsize(Face::Sans, 24, "Narrated by " + now.narrator, w), t.muted);
            y += 44;
        }
        y += 40;
        const double pos = p.position(), dur = std::max(now.duration, p.duration());
        const int ci = L().chapter_at(pos);
        // The chapter, and how far into it.
        double from = 0, to = dur;
        if (ci >= 0) {
            const auto& ch = now.chapters[ci];
            from = ch.start;
            to = ch.end > ch.start ? ch.end : (ci + 1 < (int)now.chapters.size() ? now.chapters[ci + 1].start : dur);
            gfx::draw_text(c, Face::SansSemiBold, 30, x, y,
                           gfx::ellipsize(Face::SansSemiBold, 30, ch.title.empty() ? "Chapter " + std::to_string(ci + 1) : ch.title, w),
                           t.text);
            const std::string n = "Chapter " + std::to_string(ci + 1) + " of " + std::to_string(now.chapters.size());
            y += 46;
            gfx::draw_text(c, Face::Sans, 22, x, y, n, t.muted);
            y += 44;
        }
        draw_progress(c, {x, y, w, 12}, to > from ? (float)((pos - from) / (to - from)) : 0.f);
        y += 26;
        const std::string elapsed = svc::format_duration(pos - from), length = "\xE2\x88\x92" + svc::format_duration(to - pos);
        gfx::draw_text(c, Face::SansSemiBold, 26, x, y, elapsed, t.text);
        gfx::draw_text(c, Face::SansSemiBold, 26, x + w - gfx::text_width(Face::SansSemiBold, 26, length), y, length, t.text);
        y += 70;
        // The whole book, and the time left at this speed.
        draw_progress(c, {x, y, w, 6}, dur > 0 ? (float)(pos / dur) : 0.f);
        y += 20;
        const double speed = p.speed();
        std::string book = svc::format_duration(pos) + " of " + svc::format_duration(dur) + "   \xC2\xB7   " +
                           svc::format_duration((dur - pos) / speed) + " left";
        if (speed != 1) book += " at " + speed_text(speed);
        gfx::draw_text(c, Face::Sans, 23, x, y, book, t.muted);
        y += 70;
        // State, speed and sleep timer.
        std::string state;
        bool error = false;
        switch (p.state()) {
            case audio::State::Loading: state = "Loading\xE2\x80\xA6"; break;
            case audio::State::Buffering: state = "Buffering\xE2\x80\xA6"; break;
            case audio::State::Paused: state = "Paused"; break;
            case audio::State::Playing: state = "Playing"; break;
            case audio::State::Ended: state = "Finished. Press X to listen again."; break;
            case audio::State::Error:
                state = p.error() + " Press X to try again.";
                error = true;
                break;
            default: break;
        }
        if (L().starting()) state = "Starting\xE2\x80\xA6";
        const Rect pill1{x, y, 210, 56}, pill2{x + 226, y, 330, 56};
        c.fill_rounded(pill1, 28, t.surface);
        gfx::draw_text(c, Face::SansSemiBold, 25, pill1.x + 26, pill1.y + 13, "Speed " + speed_text(speed), t.text);
        c.fill_rounded(pill2, 28, t.surface);
        gfx::draw_text(c, Face::SansSemiBold, 25, pill2.x + 26, pill2.y + 13, "Sleep: " + L().sleep_label(), t.text);
        y += 90;
        if (animating()) draw_spinner(c, x + 14, y + 18, 12);
        gfx::draw_wrapped(c, Face::SansSemiBold, 26, animating() ? x + 40 : x, y, w - 40, state, error ? t.error : t.muted, 3, 36);
        draw_footer(c, hints());
    }

private:
    void bookmark() {
        const double pos = audio::player().position();
        const int ci = L().chapter_at(pos);
        std::string title = ci >= 0 && !L().now().chapters[ci].title.empty() ? L().now().chapters[ci].title : L().now().title;
        L().add_bookmark(L().now().id, pos, title + " \xC2\xB7 " + svc::format_duration(pos));
        app().toast("Bookmark added at " + svc::format_duration(pos));
    }

    void menu() {
        std::vector<MenuItem> items;
        if (!L().now().chapters.empty())
            items.push_back({"Chapters", [] { return std::to_string(L().now().chapters.size()); }, [] {
                                 std::vector<MenuItem> list;
                                 const int current = L().chapter_at(audio::player().position());
                                 for (int i = 0; i < (int)L().now().chapters.size(); ++i) {
                                     const auto& ch = L().now().chapters[i];
                                     const double start = ch.start;
                                     list.push_back({(i == current ? "\xE2\x96\xB6 " : "") + (ch.title.empty() ? "Chapter " + std::to_string(i + 1) : ch.title),
                                                     [start] { return svc::format_duration(start); }, [start] { L().seek(start); }, nullptr});
                                 }
                                 open_menu("Chapters", std::move(list));
                             },
                             nullptr});
        const auto marks = L().bookmarks(L().now().id);
        items.push_back({"Bookmarks", [n = marks.size()] { return std::to_string(n); }, [] {
                             std::vector<MenuItem> list;
                             const std::string id = L().now().id;
                             for (const auto& b : L().bookmarks(id)) {
                                 const double at = b.time;
                                 list.push_back({b.title.empty() ? svc::format_duration(at) : b.title,
                                                 [at] { return svc::format_duration(at); }, [at] { L().seek(at); }, nullptr});
                             }
                             if (!list.empty()) {
                                 MenuItem del{"Delete a bookmark\xE2\x80\xA6", nullptr, [id] {
                                                  std::vector<MenuItem> d;
                                                  for (const auto& b : L().bookmarks(id)) {
                                                      const double at = b.time;
                                                      MenuItem m{b.title.empty() ? svc::format_duration(at) : b.title,
                                                                 [at] { return svc::format_duration(at); },
                                                                 [id, at] {
                                                                     L().remove_bookmark(id, at);
                                                                     app().toast("Bookmark deleted");
                                                                 },
                                                                 nullptr};
                                                      m.destructive = true;
                                                      d.push_back(m);
                                                  }
                                                  open_menu("Delete a bookmark", std::move(d));
                                              },
                                              nullptr};
                                 del.destructive = true;
                                 list.push_back(del);
                             }
                             open_menu("Bookmarks", std::move(list),
                                       list.empty() ? "No bookmarks yet. Press Square while listening to add one." : "");
                         },
                         nullptr});
        items.push_back({"Add a bookmark here", nullptr, [this] { bookmark(); }, nullptr});
        MenuItem speed{"Speed", [] { return speed_text(audio::player().speed()); }, nullptr,
                       [](int d) { L().set_speed(audio::player().speed() + 0.05 * d); }};
        speed.stay_open = true;
        items.push_back(speed);
        MenuItem sleep{"Sleep timer", [] { return L().sleep_label(); }, nullptr, [](int d) {
                           static const int steps[] = {0, 15, 30, 45, 60, 90, -1};
                           int i = 0;
                           const std::string label = L().sleep_label();
                           for (int k = 0; k < 7; ++k)
                               if (steps[k] == sleep_choice_) i = k;
                           if (label == "Off") i = 0;
                           sleep_choice_ = steps[(i + 7 + d) % 7];
                           L().set_sleep(sleep_choice_);
                       }};
        sleep.stay_open = true;
        items.push_back(sleep);
        MenuItem stop{"Stop playback", nullptr, [] {
                          L().stop();
                          app().pop();
                      },
                      nullptr};
        stop.destructive = true;
        items.push_back(stop);
        open_menu(L().now().title, std::move(items), L().now().author);
    }

    static int sleep_choice_;
};

int PlayerScreen::sleep_choice_ = 0;

}  // namespace

std::unique_ptr<Screen> make_audiobooks_screen() { return std::make_unique<AudiobooksScreen>(); }
void open_audiobook(const abs::Book& book) {
    if (lib::starts_with(book.id, kTalkingPrefix)) {
        request_read_aloud();
        open_file(book.id.substr(strlen(kTalkingPrefix)), book.title, book.author);
        return;
    }
    app().push(std::make_unique<AudiobookScreen>(book));
}
void open_player() { app().push(std::make_unique<PlayerScreen>()); }

void draw_now_playing(gfx::Canvas& c) {
    if (!L().active()) return;
    const Theme& t = theme();
    const svc::NowPlaying& now = L().now();
    audio::Player& p = audio::player();
    const Rect r{kMarginX - 16, kFooterTop + 6, 700, 68};
    c.fill_rounded(r, 16, t.surface);
    draw_cover(c, {r.x + 8, r.y + 8, 52, 52}, now.cover, now.title, now.author);
    const bool playing = p.state() == audio::State::Playing;
    // A play / pause symbol.
    const int ix = r.x + 76, iy = r.y + 22;
    if (playing) {
        c.fill({ix, iy, 7, 24}, t.accent);
        c.fill({ix + 13, iy, 7, 24}, t.accent);
    } else {
        play_glyph(c, ix, iy, 24, t.muted);
    }
    const std::string time = svc::format_duration(p.position()) + " / " + svc::format_duration(now.duration);
    const int tw = gfx::text_width(Face::Sans, 21, time);
    gfx::draw_text(c, Face::SansSemiBold, 23, ix + 36, r.y + 8, gfx::ellipsize(Face::SansSemiBold, 23, now.title, r.w - 140 - tw), t.text);
    gfx::draw_text(c, Face::Sans, 21, ix + 36, r.y + 38, gfx::ellipsize(Face::Sans, 21, now.author, r.w - 140 - tw), t.muted);
    gfx::draw_text(c, Face::Sans, 21, r.right() - 20 - tw, r.y + 22, time, t.muted);
}

}  // namespace ui
