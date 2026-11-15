// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The Kiwix online library (library.kiwix.org): browsing its collections,
// downloading them to a USB drive or the console, or handing them to the
// download helper beside the user's Kiwix Server; and the list of downloads.
#include <algorithm>

#include "core/fs.h"
#include "core/text.h"
#include "gfx/font.h"
#include "net/url.h"
#include "platform/platform.h"
#include "services/settings.h"
#include "services/zim_downloads.h"
#include "services/zim_shelf.h"
#include "ui/app.h"
#include "ui/kiwix_screens.h"
#include "ui/overlays.h"
#include "ui/troubleshoot.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

// What is being browsed, kept between visits for the session.
struct Browse {
    svc::kiwix::OnlineQuery query;
    std::vector<svc::kiwix::Facet> languages, categories;
    Browse() { query.language = "eng"; }
};
Browse& browse() {
    static Browse b;
    return b;
}

std::string category_label(const std::string& code) {
    if (code.empty()) return "All categories";
    std::string s = code;
    for (char& ch : s)
        if (ch == '_') ch = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
    return s;
}

std::string language_label(const std::string& code) {
    if (code.empty()) return "All languages";
    for (const auto& f : browse().languages)
        if (f.code == code) return f.title;
    return kiwix_language_name(code);
}

std::string percent(uint64_t done, uint64_t total) {
    if (!total) return lib::format_bytes(done);
    return lib::format_bytes(done) + " of " + lib::format_bytes(total) + " (" +
           std::to_string((int)(done * 100 / std::max<uint64_t>(total, 1))) + "%)";
}

void open_downloads();

// ---- a collection of the online library ----------------------------------------------

class OnlineBookScreen : public Screen {
public:
    explicit OnlineBookScreen(svc::ZimBook book) : book_(std::move(book)) { refresh(); }

    Tab tab() const override { return Tab::Knowledge; }
    void resumed() override { refresh(); }

    std::vector<Hint> hints() const override { return {{kCircle, "Back"}, {kOptions, "Downloads"}, {kCross, "Select"}}; }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (in.press(kOptions)) {
            open_downloads();
            return;
        }
        // A download of this collection moving on: its line changes.
        if (svc::zim_downloads().generation() != generation_) {
            generation_ = svc::zim_downloads().generation();
            if (now_ms() - last_refresh_ > 1000 || !anything_running()) refresh();
            else platform::request_redraw();
        }
        nav_.update(in, (int)rows_.size(), kVisible);
        if (in.press(kCross) && nav_.index < (int)rows_.size() && rows_[nav_.index].action) rows_[nav_.index].action();
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        const Rect info{kMarginX, kContentTop, 620, kFooterTop - kContentTop - 30};
        draw_panel(c, info);
        draw_kiwix_icon(c, {info.x + 40, info.y + 40, 112, 112}, book_);
        int y = info.y + 180;
        y += gfx::draw_wrapped(c, Face::SansBold, 36, info.x + 40, y, info.w - 80, book_.title, t.text, 2) + 10;
        y += gfx::draw_wrapped(c, Face::Sans, 24, info.x + 40, y, info.w - 80, book_.summary, t.muted, 3, 33) + 20;
        auto fact = [&](const std::string& k, const std::string& v) {
            if (v.empty()) return;
            gfx::draw_text(c, Face::Sans, 22, info.x + 40, y, k, t.faint);
            gfx::draw_text(c, Face::SansSemiBold, 22, info.x + 200, y, gfx::ellipsize(Face::SansSemiBold, 22, v, info.w - 240), t.text);
            y += 36;
        };
        fact("Size", book_.size ? lib::format_bytes(book_.size) : std::string());
        fact("Articles", book_.articles ? lib::format_count(book_.articles) : std::string());
        fact("Media", book_.media ? lib::format_count(book_.media) : std::string());
        fact("Language", language_label(book_.language));
        fact("Updated", book_.updated);
        fact("Variant", book_.flavour);
        fact("File", book_.file_name());
        y += 14;
        gfx::draw_wrapped(c, Face::Sans, 21, info.x + 40, y, info.w - 80,
                          "A downloaded collection appears in the Knowledge section and is read without a server "
                          "(title search; no full-text search). It can also be copied into your server's folder.",
                          t.faint, 4, 30);

        const int lx = info.right() + 40, lw = kWidth - kMarginX - lx;
        int ly = kContentTop;
        if (loading_ && rows_.empty()) {
            draw_loading(c, {lx, ly, lw, 300}, "Looking for earlier downloads");
            return;
        }
        for (int i = nav_.top; i < std::min((int)rows_.size(), nav_.top + kVisible); ++i) {
            const Row& row = rows_[i];
            const Rect r{lx, ly, lw, kRowH - 14};
            const bool on = i == nav_.index;
            draw_panel(c, r, on);
            if (on) draw_focus(c, r, 14);
            gfx::draw_text(c, Face::SansSemiBold, 28, r.x + 28, r.y + 14,
                           gfx::ellipsize(Face::SansSemiBold, 28, row.title, r.w - 56), row.action ? t.text : t.muted);
            std::string detail = row.detail;
            float progress = -1;
            if (row.job) {
                for (const auto& j : svc::zim_downloads().jobs())
                    if (j.id == row.job && (j.state == svc::ZimJob::State::Running || j.state == svc::ZimJob::State::Queued)) {
                        detail = (j.state == svc::ZimJob::State::Queued ? "Waiting: " : "Downloading: ") + percent(j.done, j.total);
                        progress = j.total ? (float)j.done / (float)j.total : 0.f;
                    }
            }
            gfx::draw_wrapped(c, Face::Sans, 21, r.x + 28, r.y + 54, r.w - 56, detail, t.muted, 2, 29);
            if (progress >= 0) draw_progress(c, {r.x + 28, r.bottom() - 18, r.w - 56, 6}, progress);
            ly += kRowH;
        }
        draw_scrollbar(c, {kWidth - kMarginX + 30, kContentTop, 8, kVisible * kRowH}, nav_.top, kVisible, (int)rows_.size());
    }

private:
    struct Row {
        std::string title, detail;
        std::function<void()> action;
        int job = 0;  // a download of this collection under way
    };
    static constexpr int kRowH = 136, kVisible = 6;

    bool anything_running() const {
        for (const auto& j : svc::zim_downloads().jobs())
            if (j.state == svc::ZimJob::State::Running) return true;
        return false;
    }

    // Looks at what is on the drives (a worker: USB goes through FTP on some consoles).
    void refresh() {
        last_refresh_ = now_ms();
        loading_ = true;
        struct Result {
            svc::ZimOnDisk usb, console;
            bool has_usb = false;
        };
        const svc::ZimBook b = book_;
        lib::async<Result>(
            token_,
            [b]() {
                Result r;
                r.has_usb = !svc::zim_folder(true).empty();
                if (r.has_usb) r.usb = svc::zim_on_disk(b, true);
                r.console = svc::zim_on_disk(b, false);
                return r;
            },
            [this](Result& r) {
                loading_ = false;
                build(r.has_usb, r.usb, r.console);
            });
    }

    int job_for(const std::string& path) const {
        for (const auto& j : svc::zim_downloads().jobs())
            if (j.path == path && (j.state == svc::ZimJob::State::Running || j.state == svc::ZimJob::State::Queued)) return j.id;
        return 0;
    }

    void build(bool has_usb, const svc::ZimOnDisk& usb, const svc::ZimOnDisk& console) {
        rows_.clear();
        const svc::ZimBook b = book_;
        if (svc::kiwix_helper::configured()) {
            rows_.push_back({"Send to my Kiwix Server",
                             "The download helper beside your server fetches it into the server's folder"
                             " (" + lib::format_bytes(b.size) + "); the PS4 can be turned off meanwhile.",
                             [this] { send_to_server(); }});
        } else {
            rows_.push_back({"Send to my Kiwix Server",
                             "Needs the download helper on the server: see Settings \xE2\x80\xBA Knowledge.",
                             nullptr});
        }
        auto add = [&](bool to_usb, const svc::ZimOnDisk& d) {
            const char* where = to_usb ? "the USB drive" : "this PS4";
            Row row;
            row.title = std::string("Download to ") + where;
            if (to_usb && !has_usb) {
                row.detail = "No USB drive is connected.";
                rows_.push_back(row);
                return;
            }
            row.job = job_for(d.path);
            if (d.complete) {
                row.title = std::string("Read it (downloaded to ") + where + ")";
                row.detail = d.path;
                const std::string path = d.path;
                row.action = [path] {
                    for (const auto& z : svc::zim_shelf().books())
                        if (z.file == path) {
                            open_kiwix_collection(z);
                            return;
                        }
                    svc::zim_shelf().scan_async();
                    app().toast("Still looking for it: try again in a moment");
                };
                rows_.push_back(row);
                return;
            }
            if (d.partial && !row.job) {
                row.title = std::string("Continue the download to ") + where;
                row.detail = percent(d.partial, b.size) + " done. ";
            }
            row.detail += "Saved in " + lib::parent_dir(d.path) + ".";
            if (to_usb && b.size > svc::kFat32Limit)
                row.detail += " Over 4 GB: the drive must be exFAT (FAT32 holds files up to 4 GB).";
            if (!to_usb) row.detail += " The PS4's free space is not known: check it first.";
            if (!row.job) row.action = [this, to_usb] { download(to_usb); };
            rows_.push_back(row);
        };
        add(true, usb);
        add(false, console);
        rows_.push_back({"Downloads", "Everything downloading on this PS4 and on your server", [] { open_downloads(); }});
        nav_.clamp((int)rows_.size(), kVisible);
    }

    void download(bool to_usb) {
        const svc::ZimBook b = book_;
        auto go = [this, b, to_usb] {
            std::string error;
            if (!svc::zim_downloads().start(b, to_usb, error)) {
                app().toast(error);
                return;
            }
            app().toast("Downloading: OPTIONS shows the downloads");
            refresh();
        };
        if (b.size >= 1024ull * 1024 * 1024) {
            confirm("Download " + lib::format_bytes(b.size) + "?",
                    "Large collections take a long time. Librarian keeps downloading while it is open; if it stops, "
                    "starting the download again continues where it was.",
                    "Download", go);
        } else {
            go();
        }
    }

    void send_to_server() {
        const svc::ZimBook b = book_;
        app().toast("Sending to the helper\xE2\x80\xA6");
        lib::async<std::string>(
            token_,
            [b]() {
                std::string error;
                return svc::kiwix_helper::send(b, error) ? std::string() : (error.empty() ? std::string("Failed.") : error);
            },
            [](std::string& error) {
                app().toast(error.empty() ? std::string("Your server is downloading it: OPTIONS shows the downloads") : error);
            });
    }

    svc::ZimBook book_;
    std::vector<Row> rows_;
    ListNav nav_;
    bool loading_ = false;
    uint64_t generation_ = 0, last_refresh_ = 0;
};

// ---- the online library ----------------------------------------------------------------

class OnlineLibraryScreen : public Screen {
public:
    explicit OnlineLibraryScreen(bool root) : root_(root) { reload(); }

    Tab tab() const override { return Tab::Knowledge; }
    bool animating() const override { return loading_; }

    std::vector<Hint> hints() const override {
        std::vector<Hint> h;
        if (!root_) h.push_back({kCircle, "Back"});
        h.push_back({kOptions, "Downloads"});
        h.push_back({kTriangle, "Filter"});
        h.push_back({kSquare, "Search"});
        if (!books_.empty()) h.push_back({kCross, "Open"});
        else if (!loading_ && !error_.empty()) h.push_back({kCross, "Try again"});
        return h;
    }

    void update(const Input& in) override {
        if (!root_ && in.press(kCircle)) {
            app().pop();
            return;
        }
        if (in.press(kOptions)) {
            open_downloads();
            return;
        }
        if (in.press(kTriangle)) {
            filter_menu();
            return;
        }
        if (in.press(kSquare)) {
            open_keyboard("Search the Kiwix online library", browse().query.text, KeyboardKind::Text, [this](const std::string& v) {
                browse().query.text = lib::trim(v);
                reload();
            });
            return;
        }
        if (books_.empty()) {
            if (in.press(kCross) && !loading_) reload();
            return;
        }
        nav_.update(in, (int)books_.size(), kCols, kRows);
        // More as the focus nears the end of what is loaded.
        if (!loading_ && (int)books_.size() < total_ && nav_.index >= (int)books_.size() - kCols * 2) load_more();
        if (in.press(kCross)) app().push(std::make_unique<OnlineBookScreen>(books_[nav_.index]));
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        const Rect area{kMarginX, kContentTop + 90, kContentW, kFooterTop - kContentTop - 90};
        const char* title = "Kiwix online library";
        gfx::draw_text(c, Face::SansBold, kTitlePx, kMarginX, kContentTop, title, t.text);
        const auto& q = browse().query;
        std::string line = language_label(q.language) + "  \xC2\xB7  " + category_label(q.category);
        if (!q.text.empty()) line += "  \xC2\xB7  \xE2\x80\x9C" + q.text + "\xE2\x80\x9D";
        if (!books_.empty()) line += "  \xC2\xB7  " + std::to_string(total_) + " collections";
        const int tx = kMarginX + gfx::text_width(Face::SansBold, kTitlePx, title) + 28;
        gfx::draw_text(c, Face::Sans, kSmallPx, tx, kContentTop + 18,
                       gfx::ellipsize(Face::Sans, kSmallPx, line, kWidth - kMarginX - tx), t.muted);
        if (books_.empty()) {
            if (loading_) draw_loading(c, area, "Loading the Kiwix online library");
            else if (!error_.empty())
                draw_message(c, area, "Could not reach the Kiwix online library",
                             error_ + " It is at library.kiwix.org (HTTPS); check the PS4's internet connection. "
                                      "Press X to try again.",
                             true);
            else
                draw_message(c, area, "Nothing found",
                             "No collection matches. Press Triangle to change the language or category, or Square to "
                             "search for something else.");
            return;
        }
        const int gap = 24, card_w = (kContentW - (kCols - 1) * gap) / kCols, card_h = 222;
        const int top = kContentTop + 84;
        for (int i = nav_.top_row * kCols; i < std::min((int)books_.size(), (nav_.top_row + kRows) * kCols); ++i) {
            const int row = i / kCols - nav_.top_row, col = i % kCols;
            const Rect r{kMarginX + col * (card_w + gap), top + row * (card_h + gap), card_w, card_h};
            const svc::ZimBook& b = books_[i];
            const bool on = i == nav_.index;
            draw_panel(c, r, on);
            if (on) draw_focus(c, r, 14);
            draw_kiwix_icon(c, {r.x + 24, r.y + 24, 96, 96}, b);
            const int x = r.x + 144, w = r.w - 168;
            int y = r.y + 26;
            for (const auto& l : gfx::wrap_text(Face::SansSemiBold, 29, b.title, w, 2)) {
                gfx::draw_text(c, Face::SansSemiBold, 29, x, y, l, t.text);
                y += 38;
            }
            std::string meta = lib::format_bytes(b.size);
            if (!b.flavour.empty()) meta += "  \xC2\xB7  " + b.flavour;
            if (q.language.empty() && !b.language.empty()) meta += "  \xC2\xB7  " + language_label(b.language);
            gfx::draw_text(c, Face::Sans, 21, x, y + 4, gfx::ellipsize(Face::Sans, 21, meta, w), t.muted);
            gfx::draw_wrapped(c, Face::Sans, 22, r.x + 24, r.y + 138, r.w - 48, b.summary, t.muted, 2, 31);
        }
        const int total_rows = ((int)books_.size() + kCols - 1) / kCols;
        draw_scrollbar(c, {kWidth - kMarginX + 30, top, 8, kRows * (card_h + gap) - gap}, nav_.top_row, kRows, total_rows);
        if (loading_) draw_spinner(c, kWidth - kMarginX - 20, kContentTop + 26, 14);
    }

private:
    static constexpr int kCols = 3, kRows = 3;

    void reload() {
        books_.clear();
        total_ = 0;
        nav_ = GridNav();
        ++generation_;
        load_more();
    }

    void load_more() {
        loading_ = true;
        error_.clear();
        struct Result {
            std::vector<svc::ZimBook> books;
            std::vector<svc::kiwix::Facet> languages, categories;
            std::string error;
            int total = 0;
            bool ok = false;
        };
        svc::kiwix::OnlineQuery q = browse().query;
        q.start = (int)books_.size();
        const bool facets = browse().languages.empty();
        const int generation = generation_;
        lib::async<Result>(
            token_,
            [q, facets]() {
                Result r;
                r.ok = svc::kiwix::online_catalog(q, r.books, r.total, r.error);
                if (r.ok && facets) {
                    std::string ignored;
                    svc::kiwix::online_languages(r.languages, ignored);
                    svc::kiwix::online_categories(r.categories, ignored);
                }
                return r;
            },
            [this, generation](Result& r) {
                if (generation != generation_) return;  // the filter changed since
                loading_ = false;
                if (!r.ok) {
                    error_ = r.error;
                    return;
                }
                if (!r.languages.empty()) browse().languages = std::move(r.languages);
                if (!r.categories.empty()) browse().categories = std::move(r.categories);
                total_ = r.total;
                for (auto& b : r.books) books_.push_back(std::move(b));
                if (r.books.empty()) total_ = (int)books_.size();  // nothing more to load
                nav_.clamp((int)books_.size(), kCols, kRows);
            });
    }

    void filter_menu() {
        std::vector<MenuItem> items;
        items.push_back({"Language", [] { return language_label(browse().query.language); }, [this] { language_menu(); }, nullptr});
        items.push_back({"Category", [] { return category_label(browse().query.category); }, [this] { category_menu(); }, nullptr});
        if (!browse().query.text.empty())
            items.push_back({"Clear the search", [] { return browse().query.text; }, [this] {
                                 browse().query.text.clear();
                                 reload();
                             },
                             nullptr});
        if (root_) {
            items.push_back({"Use my own Kiwix Server\xE2\x80\xA6", nullptr, [] {
                                 open_keyboard("Kiwix Server address (e.g. 192.168.1.10:8080)", "http://", KeyboardKind::Url,
                                               [](const std::string& v) {
                                                   const std::string url = lib::normalise_server_url(v);
                                                   if (url.empty()) return;
                                                   svc::settings().kiwix_url = url;
                                                   svc::settings_save();
                                                   app().switch_tab(Tab::Knowledge);
                                               });
                             },
                             nullptr});
        }
        open_menu("Kiwix online library", std::move(items), "library.kiwix.org");
    }

    void language_menu() {
        std::vector<MenuItem> items;
        auto pick = [this](const std::string& code) {
            browse().query.language = code;
            reload();
        };
        items.push_back({"All languages", nullptr, [pick] { pick(""); }, nullptr});
        // The most used first, then all of them.
        static const char* kCommon[] = {"eng", "fra", "deu", "spa", "ita", "por", "rus", "ara", "zho", "jpn"};
        for (const char* code : kCommon) {
            const std::string c = code;
            items.push_back({language_label(c), nullptr, [pick, c] { pick(c); }, nullptr});
        }
        for (const auto& f : browse().languages) {
            if (std::find(std::begin(kCommon), std::end(kCommon), f.code) != std::end(kCommon)) continue;
            const std::string c = f.code;
            items.push_back({f.title, [n = f.count] { return std::to_string(n); }, [pick, c] { pick(c); }, nullptr});
        }
        open_menu("Language", std::move(items));
    }

    void category_menu() {
        std::vector<MenuItem> items;
        auto pick = [this](const std::string& code) {
            browse().query.category = code;
            reload();
        };
        items.push_back({"All categories", nullptr, [pick] { pick(""); }, nullptr});
        for (const auto& f : browse().categories) {
            const std::string c = f.code;
            items.push_back({category_label(c), nullptr, [pick, c] { pick(c); }, nullptr});
        }
        open_menu("Category", std::move(items));
    }

    bool root_;
    std::vector<svc::ZimBook> books_;
    int total_ = 0, generation_ = 0;
    bool loading_ = false;
    std::string error_;
    GridNav nav_;
};

// ---- downloads ---------------------------------------------------------------------------

class DownloadsScreen : public Screen {
public:
    DownloadsScreen() { poll_helper(); }

    Tab tab() const override { return Tab::Knowledge; }
    std::vector<Hint> hints() const override { return {{kCircle, "Back"}, {kCross, "Select"}}; }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (svc::zim_downloads().generation() != generation_) {
            generation_ = svc::zim_downloads().generation();
            platform::request_redraw();
        }
        // The server's downloads, every few seconds.
        if (svc::kiwix_helper::configured() && !polling_ && now_ms() - last_poll_ > 3000) poll_helper();
        const auto rows = build();
        nav_.update(in, (int)rows.size(), kVisible);
        if (in.press(kCross) && nav_.index < (int)rows.size() && rows[nav_.index].action) rows[nav_.index].action();
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        gfx::draw_text(c, Face::SansBold, kTitlePx, kMarginX, kContentTop, "Collection downloads", t.text);
        const auto rows = build();
        nav_.clamp((int)rows.size(), kVisible);
        int y = kContentTop + 84;
        if (rows.empty()) {
            draw_message(c, {kMarginX, y, kContentW, kFooterTop - y}, "No downloads",
                         "Collections you download from the Kiwix online library are listed here while they download.");
            return;
        }
        for (int i = nav_.top; i < std::min((int)rows.size(), nav_.top + kVisible); ++i) {
            const Line& row = rows[i];
            if (row.header) {
                gfx::draw_text(c, Face::SansSemiBold, 24, kMarginX + 8, y + 30, row.title, t.muted);
                y += 76;
                continue;
            }
            const Rect r{kMarginX, y, kContentW, kRowH - 14};
            const bool on = i == nav_.index;
            draw_panel(c, r, on);
            if (on) draw_focus(c, r, 14);
            gfx::draw_text(c, Face::SansSemiBold, 28, r.x + 28, r.y + 14, gfx::ellipsize(Face::SansSemiBold, 28, row.title, r.w - 56), t.text);
            gfx::draw_text(c, Face::Sans, 21, r.x + 28, r.y + 54, gfx::ellipsize(Face::Sans, 21, row.detail, r.w - 56),
                           row.failed ? t.error : t.muted);
            if (row.progress >= 0) draw_progress(c, {r.x + 28, r.bottom() - 20, r.w - 56, 6}, row.progress);
            y += kRowH;
        }
        draw_scrollbar(c, {kWidth - kMarginX + 30, kContentTop + 84, 8, kVisible * kRowH}, nav_.top, kVisible, (int)rows.size());
    }

private:
    struct Line {
        std::string title, detail;
        std::function<void()> action;
        float progress = -1;
        bool header = false, failed = false;
    };
    static constexpr int kRowH = 118, kVisible = 6;

    std::vector<Line> build() {
        std::vector<Line> out;
        const auto local = svc::zim_downloads().jobs();
        if (!local.empty()) out.push_back({"On this PS4", "", nullptr, -1, true});
        for (auto it = local.rbegin(); it != local.rend(); ++it) {
            const svc::ZimJob j = *it;
            Line l;
            l.title = j.title.empty() ? lib::file_name(j.path) : j.title;
            using S = svc::ZimJob::State;
            switch (j.state) {
                case S::Queued: l.detail = "Waiting  \xC2\xB7  " + lib::parent_dir(j.path); break;
                case S::Running:
                    l.detail = percent(j.done, j.total) + "  \xC2\xB7  " + lib::parent_dir(j.path);
                    l.progress = j.total ? (float)j.done / (float)j.total : 0.f;
                    break;
                case S::Done: l.detail = "Downloaded: " + j.path; break;
                case S::Cancelled: l.detail = "Cancelled"; break;
                case S::Failed:
                    l.detail = j.error + " (" + percent(j.done, j.total) + "; select to continue)";
                    l.failed = true;
                    break;
            }
            if (j.state == S::Queued || j.state == S::Running) {
                l.action = [j] {
                    confirm("Cancel this download?", "What has been downloaded of it is deleted.", "Cancel download",
                            [id = j.id] { svc::zim_downloads().cancel(id); });
                };
            } else if (j.state == S::Failed || j.state == S::Cancelled) {
                l.action = [j] {
                    svc::ZimBook b;
                    b.title = j.title;
                    b.download = j.url;
                    b.size = j.total;
                    std::string error;
                    if (!svc::zim_downloads().start(b, j.on_usb, error)) app().toast(error);
                };
            }
            out.push_back(std::move(l));
        }
        if (svc::kiwix_helper::configured()) {
            out.push_back({"On your Kiwix Server", "", nullptr, -1, true});
            if (!helper_error_.empty()) out.push_back({"The download helper did not answer", helper_error_, nullptr, -1, false, true});
            else if (helper_jobs_.empty()) out.push_back({polling_ && !polled_ ? "Asking the helper\xE2\x80\xA6" : "Nothing downloading", "", nullptr});
            for (const auto& j : helper_jobs_) {
                Line l;
                l.title = j.title.empty() ? j.file : j.title;
                if (j.state == "running") {
                    l.detail = percent(j.done, j.total);
                    l.progress = j.total ? (float)j.done / (float)j.total : 0.f;
                } else if (j.state == "done") {
                    l.detail = "Downloaded: " + j.file;
                } else if (j.state == "failed") {
                    l.detail = j.error;
                    l.failed = true;
                } else {
                    l.detail = j.state == "queued" ? "Waiting" : "Cancelled";
                }
                if (j.state == "running" || j.state == "queued") {
                    const std::string id = j.id;
                    l.action = [this, id] {
                        confirm("Cancel this download on the server?", "The helper deletes what it has downloaded of it.",
                                "Cancel download", [this, id] { cancel_helper(id); });
                    };
                }
                out.push_back(std::move(l));
            }
        }
        return out;
    }

    void poll_helper() {
        if (!svc::kiwix_helper::configured()) return;
        polling_ = true;
        last_poll_ = now_ms();
        struct Result {
            std::vector<svc::kiwix_helper::Job> jobs;
            std::string error;
        };
        lib::async<Result>(
            token_,
            []() {
                Result r;
                if (!svc::kiwix_helper::jobs(r.jobs, r.error) && r.error.empty()) r.error = "No answer.";
                return r;
            },
            [this](Result& r) {
                polling_ = false;
                polled_ = true;
                last_poll_ = now_ms();
                helper_error_ = r.error;
                helper_jobs_ = std::move(r.jobs);
            });
    }

    void cancel_helper(const std::string& id) {
        lib::async<std::string>(
            token_,
            [id]() {
                std::string error;
                svc::kiwix_helper::cancel(id, error);
                return error;
            },
            [this](std::string& error) {
                if (!error.empty()) app().toast(error);
                poll_helper();
            });
    }

    std::vector<svc::kiwix_helper::Job> helper_jobs_;
    std::string helper_error_;
    bool polling_ = false, polled_ = false;
    uint64_t last_poll_ = 0, generation_ = 0;
    ListNav nav_;
};

void open_downloads() { app().push(std::make_unique<DownloadsScreen>()); }

}  // namespace

void open_kiwix_online_library() { app().push(std::make_unique<OnlineLibraryScreen>(false)); }

std::unique_ptr<Screen> make_kiwix_online_root() { return std::make_unique<OnlineLibraryScreen>(true); }

}  // namespace ui
