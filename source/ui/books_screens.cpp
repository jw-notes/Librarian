// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The Books tab: Calibre-Web's catalogue through OPDS, a book's details page,
// downloads, and search.
#include <algorithm>
#include <functional>

#include "core/fs.h"
#include "core/text.h"
#include "gfx/font.h"
#include "gfx/image_cache.h"
#include "net/url.h"
#include "services/calibre.h"
#include "services/library.h"
#include "services/local_library.h"
#include "services/opds.h"
#include "services/settings.h"
#include "ui/app.h"
#include "ui/openers.h"
#include "ui/overlays.h"
#include "ui/troubleshoot.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

void open_book_details(const svc::OpdsEntry& entry);
void open_feed(const std::string& url, const std::string& title);

namespace {

bool is_downloaded(const svc::OpdsEntry& e, std::string* path = nullptr) {
    for (const auto& f : e.formats) {
        const std::string p = svc::calibre::download_path(e, f.format);
        if (lib::file_exists(p)) {
            if (path) *path = p;
            return true;
        }
    }
    return false;
}

// ---- a feed, as a cover grid (books) or a list (sections) -----------------------

class FeedView {
public:
    explicit FeedView(lib::CancelToken token) : token_(std::move(token)) {}

    void load(const std::string& url) {
        url_ = url;
        feed_ = svc::OpdsFeed();
        error_.clear();
        grid_ = GridNav();
        list_ = ListNav();
        loading_ = true;
        fetch(url, false);
    }

    // Shows nothing (another catalogue is being opened); a load in flight is dropped.
    void clear() {
        url_.clear();
        feed_ = svc::OpdsFeed();
        error_.clear();
        loading_ = false;
        ++generation_;
    }

    bool loading() const { return loading_; }
    bool empty() const { return feed_.entries.empty(); }
    bool books() const {
        for (const auto& e : feed_.entries)
            if (!e.navigation) return true;
        return false;
    }
    const svc::OpdsEntry* focused() const {
        const int i = books() ? grid_.index : list_.index;
        return i >= 0 && i < (int)feed_.entries.size() ? &feed_.entries[i] : nullptr;
    }
    const std::string& title() const { return feed_.title; }

    // Returns true if the input moved focus inside the view.
    bool update(const Input& in, bool allow_left_exit) {
        const int n = (int)feed_.entries.size();
        if (n == 0) return false;
        bool moved;
        if (books()) {
            if (allow_left_exit && in.repeat(kLeft) && grid_.index % cols_ == 0) return false;
            moved = grid_.update(in, n, cols_, rows_);
            if (grid_.index >= n - cols_ * 2) more();
        } else {
            if (allow_left_exit && in.repeat(kLeft)) return false;
            moved = list_.update(in, n, visible_, false);
            if (list_.index >= n - visible_) more();
        }
        if (in.press(kCross)) activate();
        return moved;
    }

    void activate() {
        const svc::OpdsEntry* e = focused();
        if (!e) return;
        if (e->navigation) open_feed(e->link, e->title);
        else open_book_details(*e);
    }

    void draw(gfx::Canvas& c, const Rect& area, bool focused_view) {
        const Theme& t = theme();
        if (feed_.entries.empty()) {
            if (loading_) draw_loading(c, area, "Loading\xE2\x80\xA6");
            else if (!error_.empty()) draw_message(c, area, "Could not load this list", error_, true);
            else draw_message(c, area, "Nothing here", "This list is empty.");
            return;
        }
        if (books()) {
            const int gap = 30;
            cols_ = std::max(2, (area.w + gap) / (196 + gap));
            const int card_w = (area.w - (cols_ - 1) * gap) / cols_, cover_h = card_w * 3 / 2, row_h = cover_h + 84;
            rows_ = std::max(1, (area.h + 20) / row_h);
            grid_.clamp((int)feed_.entries.size(), cols_, rows_);
            for (int i = grid_.top_row * cols_; i < std::min((int)feed_.entries.size(), (grid_.top_row + rows_) * cols_); ++i) {
                const svc::OpdsEntry& e = feed_.entries[i];
                const int row = i / cols_ - grid_.top_row, col = i % cols_;
                const Rect cover{area.x + col * (card_w + gap), area.y + row * row_h, card_w, cover_h};
                const bool on = focused_view && i == grid_.index;
                draw_cover(c, cover, e.thumbnail, e.title, e.author);
                if (is_downloaded(e)) {
                    c.fill_circle(cover.right() - 22, cover.y + 22, 14, t.success);
                    c.line(cover.right() - 29, cover.y + 22, cover.right() - 23, cover.y + 28, 3, gfx::rgb(0xFFFFFF));
                    c.line(cover.right() - 23, cover.y + 28, cover.right() - 14, cover.y + 16, 3, gfx::rgb(0xFFFFFF));
                }
                if (on) draw_focus(c, cover, 10);
                gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 22, cover.x, cover.bottom() + 12,
                               gfx::ellipsize(Face::SansSemiBold, 22, e.title, cover.w), on ? t.text : t.muted);
                gfx::draw_text(c, Face::Sans, 19, cover.x, cover.bottom() + 44, gfx::ellipsize(Face::Sans, 19, e.author, cover.w), t.faint);
            }
            const int total_rows = ((int)feed_.entries.size() + cols_ - 1) / cols_;
            draw_scrollbar(c, {area.right() + 24, area.y, 8, area.h}, grid_.top_row, rows_, total_rows);
        } else {
            const int row_h = 86;
            visible_ = std::max(1, area.h / row_h);
            list_.clamp((int)feed_.entries.size(), visible_);
            for (int i = list_.top; i < std::min((int)feed_.entries.size(), list_.top + visible_); ++i) {
                const svc::OpdsEntry& e = feed_.entries[i];
                const Rect r{area.x, area.y + (i - list_.top) * row_h, area.w, row_h - 10};
                const bool on = focused_view && i == list_.index;
                draw_panel(c, r, on);
                if (on) draw_focus(c, r, 14);
                gfx::draw_text(c, Face::SansSemiBold, 27, r.x + 26, r.y + 10, gfx::ellipsize(Face::SansSemiBold, 27, e.title, r.w - 52), t.text);
                if (!e.summary.empty())
                    gfx::draw_text(c, Face::Sans, 20, r.x + 26, r.y + 46, gfx::ellipsize(Face::Sans, 20, e.summary, r.w - 52), t.muted);
            }
            draw_scrollbar(c, {area.right() + 24, area.y, 8, area.h}, list_.top, visible_, (int)feed_.entries.size());
        }
        if (loading_) draw_spinner(c, area.right() - 20, area.y - 40, 14);
    }

private:
    void more() {
        if (loading_ || feed_.next.empty() || feed_.next == last_next_) return;
        last_next_ = feed_.next;
        loading_ = true;
        fetch(feed_.next, true);
    }

    void fetch(const std::string& url, bool append) {
        struct Result {
            svc::OpdsFeed feed;
            std::string error;
            bool ok = false;
        };
        const int generation = ++generation_;
        lib::async<Result>(
            token_,
            [url]() {
                Result r;
                r.ok = svc::opds_fetch(url, r.feed, r.error);
                return r;
            },
            [this, append, generation](Result& r) {
                if (generation != generation_) return;
                loading_ = false;
                if (!r.ok) {
                    if (!append) error_ = r.error;
                    return;
                }
                if (append) {
                    for (auto& e : r.feed.entries) feed_.entries.push_back(std::move(e));
                    feed_.next = r.feed.next;
                } else {
                    feed_ = std::move(r.feed);
                }
            });
    }

    lib::CancelToken token_;
    std::string url_, error_, last_next_;
    svc::OpdsFeed feed_;
    GridNav grid_;
    ListNav list_;
    int cols_ = 6, rows_ = 2, visible_ = 8, generation_ = 0;
    bool loading_ = false;
};

void ask_search() {
    if (svc::calibre::search_url("x").empty()) {
        app().toast("This catalogue has no search");
        return;
    }
    open_keyboard("Search the catalogue", "", KeyboardKind::Text, [](const std::string& q) {
        if (lib::trim(q).empty()) return;
        open_feed(svc::calibre::search_url(lib::trim(q)), "\xE2\x80\x9C" + lib::trim(q) + "\xE2\x80\x9D");
    });
}

}  // namespace

// ---- catalogues ----------------------------------------------------------------------

namespace {

// Project Gutenberg's entry point as it lists it (ebooks/offline_catalogs.html,
// checked 2026-11-14). Its XML feeds are to be retired in 2027 for OPDS 2
// (JSON), which Librarian does not read yet.
constexpr const char* kGutenbergName = "Project Gutenberg";
constexpr const char* kGutenbergUrl = "https://www.gutenberg.org/ebooks/search.opds/";

void add_catalogue(const std::string& name, const std::string& url, const std::function<void()>& added) {
    svc::Settings& s = svc::settings();
    bool known = false;
    for (const auto& c : s.opds_catalogs) known = known || c.url == url;
    if (!known) s.opds_catalogs.push_back({name, url, "", ""});
    s.books_catalog = url;  // shown next in the Books section
    svc::settings_save();
    svc::settings_apply();
    app().toast(known ? "That catalogue is already in the list" : "Added: it is in the Books section");
    if (added) added();
}

}  // namespace

void open_add_catalogue_menu(std::function<void()> added) {
    std::vector<MenuItem> items;
    items.push_back({kGutenbergName, nullptr, [added] { add_catalogue(kGutenbergName, kGutenbergUrl, added); }, nullptr});
    items.push_back({"Type its address\xE2\x80\xA6", nullptr, [added] {
                         open_keyboard("Catalogue address (its OPDS feed)", "http://", KeyboardKind::Url,
                                       [added](const std::string& v) {
                                           const std::string url = lib::trim(v);
                                           if (url.empty() || url == "http://" || url == "https://") return;
                                           add_catalogue("", url, added);
                                       });
                     },
                     nullptr});
    open_menu("Add a catalogue", std::move(items),
              "Free public-domain books from Project Gutenberg, or any OPDS 1 (Atom) catalogue");
}

void open_choose_catalogue_menu(std::function<void()> changed) {
    const auto all = svc::calibre::catalogs();
    const int current = svc::calibre::current_catalog();
    std::vector<MenuItem> items;
    for (int i = 0; i < (int)all.size(); ++i) {
        items.push_back({all[i].name, [i, current] { return i == current ? std::string("Showing") : std::string(); },
                         [i, changed] {
                             svc::calibre::set_current_catalog(i);
                             if (changed) changed();
                         },
                         nullptr});
    }
    items.push_back({"Add a catalogue\xE2\x80\xA6", nullptr, [changed] { open_add_catalogue_menu(changed); }, nullptr});
    open_menu("Catalogues", std::move(items), "Names, addresses and sign-in: Settings \xE2\x80\xBA Books");
}

namespace {

// ---- the Books tab -----------------------------------------------------------------

class BooksScreen : public Screen {
public:
    BooksScreen() : content_(token_) {
        if (!none()) load_root();
    }

    Tab tab() const override { return Tab::Books; }
    bool animating() const override { return content_.loading() || root_loading_; }

    std::vector<Hint> hints() const override {
        if (none()) return {{kSquare, "Set up Calibre-Web"}, {kCross, "Add a catalogue"}};
        std::vector<Hint> h;
        if (on_picker_) h = {{kTriangle, "Refresh"}, {kSquare, "Search"}, {kCross, "Change catalogue"}};
        else if (sections_.empty() && !root_loading_) h = {{kSquare, "Troubleshoot"}, {kCross, "Try again"}};
        else h = {{kTriangle, "Refresh"}, {kSquare, "Search"}, {kCross, "Open"}};
        h.push_back({kOptions, "Catalogues"});
        if (svc::calibre::catalogs().size() > 1) h.push_back({kL2, "Next"});
        return h;
    }

    void update(const Input& in) override {
        if (none()) {
            if (in.press(kCross)) open_add_catalogue_menu([this] { catalogue_changed(); });
            if (in.press(kSquare)) app().switch_tab(Tab::Settings);
            return;
        }
        if (in.press(kOptions)) {
            open_choose_catalogue_menu([this] { catalogue_changed(); });
            return;
        }
        // L2 / R2: the previous or next catalogue.
        const int count = (int)svc::calibre::catalogs().size();
        if (count > 1 && (in.press(kL2) || in.press(kR2))) {
            const int step = in.press(kR2) ? 1 : -1;
            svc::calibre::set_current_catalog((svc::calibre::current_catalog() + step + count) % count);
            catalogue_changed();
            return;
        }
        // The catalogue's name above the sections: X there picks another.
        if (on_picker_) {
            if (in.press(kCross)) open_choose_catalogue_menu([this] { catalogue_changed(); });
            else if (in.press(kTriangle)) load_root();
            else if (in.press(kSquare) && !sections_.empty()) ask_search();
            else if ((in.repeat(kDown) || in.repeat(kRight)) && !sections_.empty()) on_picker_ = false;
            return;
        }
        if (!in_content_ && !sections_.empty() && nav_.index == 0 && in.repeat(kUp)) {
            on_picker_ = true;
            return;
        }
        if (in.press(kTriangle) || (sections_.empty() && !root_loading_ && in.press(kCross))) {
            load_root();
            return;
        }
        if (in.press(kSquare)) {
            if (sections_.empty()) {
                if (!root_loading_) open_troubleshooter(Service::Calibre, root_status_, root_error_);
            } else {
                ask_search();
            }
            return;
        }
        if (in_content_) {
            if (!content_.update(in, true) && in.repeat(kLeft)) in_content_ = false;
            return;
        }
        const int before = nav_.index;
        nav_.update(in, (int)sections_.size(), kSideRows, false);
        if (nav_.index != before) select();
        if ((in.repeat(kRight) || in.press(kCross)) && !content_.empty()) in_content_ = true;
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        const Rect area{kMarginX, kContentTop, kContentW, kFooterTop - kContentTop};
        if (none()) {
            draw_message(c, area, "Add a book catalogue",
                         "Browse and download books from an online catalogue such as Project Gutenberg (free "
                         "public-domain books, no account needed) or from your own Calibre-Web server. Press X to "
                         "add a catalogue, or Square to set up Calibre-Web in Settings.");
            return;
        }
        if (sections_.empty()) {
            if (root_loading_) draw_loading(c, area, "Connecting to " + catalog_name());
            else {
                const Diagnosis d = diagnose(Service::Calibre, root_status_, root_error_);
                // The diagnosis speaks of Calibre-Web; another catalogue is named as it is.
                const auto all = svc::calibre::catalogs();
                const bool calibre = !all.empty() && all[svc::calibre::current_catalog()].calibre;
                draw_message(c, area, d.title.empty() || !calibre ? "Could not reach " + catalog_name() : d.title,
                             root_error_ + " Press Square for troubleshooting steps, or X to try again.", true);
            }
            return;
        }
        // Sections, under the catalogue's name (X there picks another).
        const int side_w = 380;
        int y = kContentTop;
        int side_rows = kSideRows - 1;
        {
            const Rect r{kMarginX, y, side_w, 62};
            draw_panel(c, r, on_picker_);
            if (on_picker_) draw_focus(c, r, 12);
            const char* more = "\xE2\x96\xBE";  // a small down-pointing triangle
            const int mw = gfx::text_width(Face::Sans, 24, more);
            gfx::draw_text(c, Face::SansBold, 26, r.x + 22, r.y + 13,
                           gfx::ellipsize(Face::SansBold, 26, catalog_name(), side_w - 66 - mw), t.accent);
            gfx::draw_text(c, Face::Sans, 24, r.right() - 22 - mw, r.y + 15, more, t.muted);
            y += 80;
        }
        nav_.clamp((int)sections_.size(), side_rows);
        for (int i = nav_.top; i < std::min((int)sections_.size(), nav_.top + side_rows); ++i) {
            const bool on = i == nav_.index;
            const Rect r{kMarginX, y, side_w, 62};
            // Dimmed while the focus is on the books or the catalogue's name.
            const bool away = in_content_ || on_picker_;
            if (on) c.fill_rounded(r, 12, away ? t.surface_hi : t.accent);
            gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 26, r.x + 22, r.y + 13,
                           gfx::ellipsize(Face::Sans, 26, sections_[i].title, side_w - 44),
                           on && !away ? t.accent_text : on ? t.text : t.muted);
            y += 68;
        }
        const Rect content{kMarginX + side_w + 50, kContentTop, kContentW - side_w - 90, kFooterTop - kContentTop - 10};
        content_.draw(c, content, in_content_);
    }

private:
    static constexpr int kSideRows = 12;

    static bool none() { return svc::calibre::catalogs().empty(); }

    void catalogue_changed() {
        in_content_ = false;
        on_picker_ = false;
        load_root();
    }

    static std::string catalog_name() {
        const auto all = svc::calibre::catalogs();
        return all.empty() ? std::string() : all[svc::calibre::current_catalog()].name;
    }

    void load_root() {
        root_loading_ = true;
        root_error_.clear();
        sections_.clear();
        content_.clear();
        struct Result {
            svc::OpdsFeed feed;
            std::string error, search, root;
            bool ok = false;
            int status = -1;
        };
        const auto all = svc::calibre::catalogs();
        const bool calibre = all.empty() || all[svc::calibre::current_catalog()].calibre;
        const std::string url = svc::calibre::root_url();
        const int generation = ++root_generation_;
        lib::async<Result>(
            token_,
            [url, calibre]() {
                Result r;
                r.root = url;
                r.ok = svc::opds_fetch(url, r.feed, r.error, &r.status);
                // Other catalogues say where their search is (often an
                // OpenSearch description, read once here).
                if (r.ok && !calibre) svc::opds_search_template(r.feed.search, r.search);
                return r;
            },
            [this, generation](Result& r) {
                if (generation != root_generation_) return;  // another catalogue since
                root_loading_ = false;
                root_status_ = r.status;
                sections_.clear();
                if (!r.ok) {
                    root_error_ = r.error;
                    return;
                }
                svc::calibre::set_search_template(r.search);
                // "Recently added" first: it is what people look for most.
                for (const auto& e : r.feed.entries)
                    if (e.navigation) sections_.push_back(e);
                // A catalogue whose first page is a list of books: one section.
                if (sections_.empty() && !r.feed.entries.empty()) {
                    svc::OpdsEntry all;
                    all.title = r.feed.title.empty() ? "All books" : r.feed.title;
                    all.link = r.root;
                    all.navigation = true;
                    sections_.push_back(all);
                }
                std::stable_sort(sections_.begin(), sections_.end(), [](const auto& a, const auto& b) {
                    return lib::ends_with(a.link, "/new") && !lib::ends_with(b.link, "/new");
                });
                nav_ = ListNav();
                select();
            });
    }

    void select() {
        if (nav_.index < (int)sections_.size()) content_.load(sections_[nav_.index].link);
    }

    FeedView content_;
    std::vector<svc::OpdsEntry> sections_;
    ListNav nav_;
    bool in_content_ = false, root_loading_ = false, on_picker_ = false;
    std::string root_error_;
    int root_status_ = 0, root_generation_ = 0;
};

// ---- a pushed feed (an author, a series, search results) -----------------------------

class FeedScreen : public Screen {
public:
    FeedScreen(const std::string& url, std::string title) : title_(std::move(title)), view_(token_) { view_.load(url); }

    Tab tab() const override { return Tab::Books; }
    bool animating() const override { return view_.loading(); }
    std::vector<Hint> hints() const override { return {{kCircle, "Back"}, {kSquare, "Search"}, {kCross, "Open"}}; }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (in.press(kSquare)) {
            ask_search();
            return;
        }
        view_.update(in, false);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        gfx::draw_text(c, Face::SansBold, 40, kMarginX, kContentTop, gfx::ellipsize(Face::SansBold, 40, title_, kContentW), t.text);
        view_.draw(c, {kMarginX, kContentTop + 76, kContentW - 40, kFooterTop - kContentTop - 86}, true);
    }

private:
    std::string title_;
    FeedView view_;
};

// ---- a book ------------------------------------------------------------------------------

class BookScreen : public Screen {
public:
    explicit BookScreen(svc::OpdsEntry entry) : e_(std::move(entry)) { build(); }

    Tab tab() const override { return Tab::Books; }
    bool animating() const override { return downloading_; }
    void resumed() override { build(); }
    std::vector<Hint> hints() const override {
        if (downloading_) return {{kCircle, "Cancel download"}};
        return {{kCircle, "Back"}, {kCross, "Select"}};
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            if (downloading_) *cancel_ = true;
            else app().pop();
            return;
        }
        if (downloading_) return;
        if (in.repeat(kUp) && action_ > 0) --action_;
        if (in.repeat(kDown) && action_ + 1 < (int)actions_.size()) ++action_;
        if (in.repeat(kRight)) summary_top_ += 3;
        if (in.repeat(kLeft)) summary_top_ = std::max(0, summary_top_ - 3);
        if (in.press(kCross) && action_ < (int)actions_.size()) actions_[action_].run();
    }

    void draw(gfx::Canvas& c) override {
        sync_progress();
        const Theme& t = theme();
        const Rect cover{kMarginX, kContentTop + 10, 440, 660};
        draw_cover(c, cover, e_.cover, e_.title, e_.author);
        const int x = cover.right() + 60, w = kWidth - kMarginX - x;
        int y = kContentTop + 4;
        y += gfx::draw_wrapped(c, Face::SerifBold, 50, x, y, w, e_.title, t.text, 2, 64) + 6;
        if (!e_.author.empty()) {
            gfx::draw_text(c, Face::Sans, 30, x, y, gfx::ellipsize(Face::Sans, 30, e_.author, w), t.accent);
            y += 52;
        }
        std::string facts;
        auto add = [&](const std::string& s) {
            if (s.empty()) return;
            if (!facts.empty()) facts += "   \xC2\xB7   ";
            facts += s;
        };
        if (!e_.series.empty()) add(e_.series);
        if (e_.issued.size() >= 4) add(e_.issued.substr(0, 4));
        add(e_.publisher);
        add(e_.language);
        std::string formats;
        for (const auto& f : e_.formats) {
            std::string up = f.format;
            for (char& ch : up) ch = (char)toupper((unsigned char)ch);
            formats += (formats.empty() ? "" : ", ") + up + (f.size ? " (" + lib::format_bytes(f.size) + ")" : "");
        }
        add(formats);
        if (!facts.empty()) {
            gfx::draw_text(c, Face::Sans, 23, x, y, gfx::ellipsize(Face::Sans, 23, facts, w), t.muted);
            y += 44;
        }
        if (!e_.categories.empty()) {
            int cx = x;
            for (const auto& cat : e_.categories) {
                const std::string label = gfx::ellipsize(Face::Sans, 20, cat, 360);
                const int cw = gfx::text_width(Face::Sans, 20, label) + 28;
                if (cx + cw > x + w) break;
                c.fill_rounded({cx, y, cw, 38}, 19, t.surface);
                gfx::draw_text(c, Face::Sans, 20, cx + 14, y + 6, label, t.muted);
                cx += cw + 10;
            }
            y += 58;
        }
        // Actions on the left of the text column, summary to their right.
        const int bw = 420;
        int by = y + 10;
        for (int i = 0; i < (int)actions_.size(); ++i) {
            const Rect r{x, by, bw, 70};
            const bool on = i == action_ && !downloading_;
            c.fill_rounded(r, 14, on ? t.accent : t.surface);
            gfx::draw_text(c, Face::SansSemiBold, 26, r.x + 26, r.y + 18, actions_[i].label, on ? t.accent_text : t.text);
            by += 82;
        }
        if (downloading_) {
            const Rect bar{x, by + 10, bw, 12};
            draw_progress(c, bar, total_ ? (float)done_ / total_ : 0.f);
            gfx::draw_text(c, Face::Sans, 22, x, by + 34,
                           "Downloading " + lib::format_bytes(done_) + (total_ ? " of " + lib::format_bytes(total_) : ""), t.muted);
        }
        if (!e_.summary.empty()) {
            const int sx = x + bw + 50, sw = x + w - sx;
            const auto lines = gfx::wrap_text(Face::Serif, 25, e_.summary, sw);
            const int line_h = 38, max_lines = (kFooterTop - 30 - (y + 10)) / line_h;
            summary_top_ = std::min(summary_top_, std::max(0, (int)lines.size() - max_lines));
            for (int i = summary_top_; i < (int)lines.size() && i < summary_top_ + max_lines; ++i)
                gfx::draw_text(c, Face::Serif, 25, sx, y + 10 + (i - summary_top_) * line_h, lines[i], t.muted);
        }
    }

private:
    struct Action {
        std::string label;
        std::function<void()> run;
    };

    void build() {
        actions_.clear();
        std::string path;
        const bool local = is_downloaded(e_, &path);
        const svc::OpdsAcquisition* preferred = svc::calibre::preferred_format(e_);
        if (local) {
            actions_.push_back({"Read", [this, path] { read(path); }});
        } else if (preferred) {
            const svc::OpdsAcquisition f = *preferred;
            actions_.push_back({"Download and read", [this, f] { download(f, true); }});
        }
        for (const auto& f : e_.formats) {
            if (preferred && f.format == preferred->format && !local) continue;
            if (!svc::calibre::readable_format(f.format)) continue;
            if (lib::file_exists(svc::calibre::download_path(e_, f.format))) continue;
            std::string up = f.format;
            for (char& ch : up) ch = (char)toupper((unsigned char)ch);
            const svc::OpdsAcquisition copy = f;
            actions_.push_back({"Download " + up, [this, copy] { download(copy, false); }});
        }
        if (local) {
            actions_.push_back({"Delete download", [this, path] {
                                    confirm("Delete the downloaded copy?", "It stays on your Calibre-Web server.", "Delete", [this, path] {
                                        lib::remove_file(path);
                                        svc::library().remove(svc::file_id(path));
                                        app().toast("Download deleted");
                                        build();
                                    });
                                }});
        }
        if (actions_.empty())
            actions_.push_back({"No readable format", [] {
                                    app().toast("Librarian reads EPUB, PDF, CBZ, CBR and TXT. Convert the book in Calibre.");
                                }});
        action_ = std::min(action_, (int)actions_.size() - 1);
    }

    void read(const std::string& path) {
        open_file(path, e_.title, e_.author, e_.cover, "calibre", e_.book_id);
    }

    void download(const svc::OpdsAcquisition& format, bool then_read) {
        downloading_ = true;
        done_ = total_ = 0;
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        auto cancel = cancel_;
        auto progress = std::make_shared<std::pair<std::atomic<uint64_t>, std::atomic<uint64_t>>>();
        progress_ = progress;
        const svc::OpdsEntry entry = e_;
        struct Result {
            bool ok = false;
            std::string path, error;
        };
        lib::async<Result>(
            token_,
            [entry, format, cancel, progress]() {
                Result r;
                r.ok = svc::calibre::download(entry, format,
                                              [cancel, progress](uint64_t done, uint64_t total) {
                                                  progress->first = done;
                                                  progress->second = total;
                                                  platform::request_redraw();
                                                  return !*cancel;
                                              },
                                              r.path, r.error);
                return r;
            },
            [this, then_read](Result& r) {
                downloading_ = false;
                if (!r.ok) {
                    app().toast(r.error);
                    return;
                }
                app().toast("Downloaded");
                // Add it to the Local Library at once and scan its folder, so it
                // appears on the shelf without a manual refresh or USB re-plug.
                svc::local_library().note_new_download(r.path);
                build();
                if (then_read) read(r.path);
            });
    }

    void sync_progress() {
        if (progress_) {
            done_ = progress_->first;
            total_ = progress_->second;
        }
    }

    svc::OpdsEntry e_;
    std::vector<Action> actions_;
    int action_ = 0, summary_top_ = 0;
    bool downloading_ = false;
    uint64_t done_ = 0, total_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::pair<std::atomic<uint64_t>, std::atomic<uint64_t>>> progress_;
};

}  // namespace

std::unique_ptr<Screen> make_books_screen() { return std::make_unique<BooksScreen>(); }
void open_feed(const std::string& url, const std::string& title) { app().push(std::make_unique<FeedScreen>(url, title)); }
void open_book_details(const svc::OpdsEntry& entry) { app().push(std::make_unique<BookScreen>(entry)); }

}  // namespace ui
