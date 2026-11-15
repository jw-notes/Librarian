// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Home: Continue Reading, Continue Listening, recent articles and favourites.
#include <algorithm>

#include "core/fs.h"
#include "core/text.h"
#include "doc/covers.h"
#include "gfx/font.h"
#include "gfx/image_cache.h"
#include "net/url.h"
#include "services/audiobookshelf.h"
#include "services/library.h"
#include "services/listening.h"
#include "services/local_library.h"
#include "services/settings.h"
#include "ui/app.h"
#include "ui/i18n.h"
#include "ui/openers.h"
#include "ui/overlays.h"
#include "ui/widgets.h"

namespace ui {

void open_audiobook(const svc::abs::Book& book);  // audiobooks_screens.cpp

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

class HomeScreen : public Screen {
public:
    HomeScreen() { reload(); }

    Tab tab() const override { return Tab::Home; }
    void resumed() override { reload(); }

    std::vector<Hint> hints() const override {
        if (rows_.empty()) return {{kOptions, "Settings"}, {kCross, "Select"}};
        return {{kTriangle, "Options"}, {kCross, "Open"}};
    }

    void update(const Input& in) override {
        // A drive plugged in or out changes which covers can be shown.
        if (svc::local_library().generation() != local_generation_ ||
            svc::listening().generation() != listening_generation_)
            reload();
        if (rows_.empty()) {
            if (in.repeat(kUp) && step_ > 0) --step_;
            if (in.repeat(kDown) && step_ < 3) ++step_;
            if (in.press(kCross)) start_step(step_);
            if (in.press(kOptions)) app().switch_tab(Tab::Settings);
            return;
        }
        if (in.repeat(kUp) && row_ > 0) --row_;
        if (in.repeat(kDown) && row_ + 1 < (int)rows_.size()) ++row_;
        Row& row = rows_[row_];
        if (in.repeat(kLeft) && row.index > 0) --row.index;
        if (in.repeat(kRight) && row.index + 1 < (int)row.items.size()) ++row.index;
        const svc::Item& item = row.items[row.index];
        if (row.audio) {
            if (in.press(kCross)) open_audiobook(audiobook_of(item));
            if (in.press(kTriangle)) audio_options(item);
            return;
        }
        if (in.press(kCross)) open_item(item);
        if (in.press(kTriangle)) options(item);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        if (rows_.empty()) {
            welcome(c);
            return;
        }
        // The focused row is kept on screen: rows scroll as a whole.
        int first = std::max(0, row_ - 1);
        if (row_ == 0) first = 0;
        int y = kContentTop - 6;
        for (int r = first; r < (int)rows_.size() && y < kFooterTop - 120; ++r) {
            const Row& row = rows_[r];
            const bool row_on = r == row_;
            gfx::draw_text(c, Face::SansBold, 32, kMarginX, y, T(row.title), row_on ? t.text : t.muted);
            y += 56;
            const int card_w = row.wide ? 520 : 236, card_h = row.wide ? 150 : 354, gap = 30;
            const int visible = (kContentW + gap) / (card_w + gap);
            const int start = std::max(0, std::min(row.index - visible + 2, (int)row.items.size() - visible));
            for (int i = std::max(0, start); i < (int)row.items.size() && i < start + visible + 1; ++i) {
                const int x = kMarginX + (i - start) * (card_w + gap);
                if (x > kWidth) break;
                const Rect r{x, y, card_w, card_h};
                const bool on = row_on && i == row.index;
                if (row.wide) article_card(c, r, row.items[i], on);
                else book_card(c, r, row.items[i], on);
            }
            y += card_h + (row.wide ? 50 : 96);
        }
    }

private:
    struct Row {
        std::string title;
        std::vector<svc::Item> items;  // copies: the library may change underneath
        int index = 0;
        bool wide = false;
        bool audio = false;  // audiobooks (Continue Listening): ids are Audiobookshelf's
    };

    static svc::abs::Book audiobook_of(const svc::Item& item) {
        svc::abs::Book b;
        b.id = item.id;
        b.title = item.title;
        b.author = item.author;
        return b;
    }

    void audio_options(const svc::Item& item) {
        const svc::Item copy = item;
        std::vector<MenuItem> items;
        items.push_back({"Open", nullptr, [copy] { open_audiobook(audiobook_of(copy)); }, nullptr});
        MenuItem hide{"Remove from Continue Listening", nullptr, [this, copy] {
                          svc::listening().hide_from_continue(copy.id);
                          reload();
                      },
                      nullptr};
        hide.destructive = true;
        items.push_back(hide);
        open_menu(item.title, std::move(items), item.author);
    }

    void reload() {
        std::vector<Row> rows;
        local_generation_ = svc::local_library().generation();
        // A local book's cover is read from its file: from where the file is
        // now, and not at all while its drive is disconnected (it would only fail).
        auto copy = [](const std::vector<const svc::Item*>& v) {
            std::vector<svc::Item> out;
            for (const svc::Item* it : v) {
                out.push_back(*it);
                svc::Item& item = out.back();
                if (item.kind == svc::ItemKind::Book && !item.path.empty())
                    item.cover = doc::relocate_cover(item.cover, item.path,
                                                     svc::local_library().locate(item.path, item.fingerprint));
            }
            return out;
        };
        const auto reading = copy(svc::library().continue_reading(20));
        if (!reading.empty()) rows.push_back({"Continue Reading", reading, 0, false});
        listening_generation_ = svc::listening().generation();
        std::vector<svc::Item> listening;
        for (const svc::RecentListen& r : svc::listening().continue_listening(20)) {
            svc::Item it;
            it.id = r.id;
            it.title = r.title;
            it.author = r.author;
            it.cover = r.cover;
            it.progress = r.duration > 0 ? (float)std::min(1.0, r.time / r.duration) : 0.0f;
            listening.push_back(std::move(it));
        }
        if (!listening.empty()) rows.push_back({"Continue Listening", listening, 0, false, true});
        const auto articles = copy(svc::library().recent_articles(20));
        if (!articles.empty()) rows.push_back({"Recent Articles", articles, 0, true});
        std::vector<svc::Item> books, saved;
        for (const svc::Item* it : svc::library().favourites()) (it->kind == svc::ItemKind::Book ? books : saved).push_back(*it);
        if (!books.empty()) rows.push_back({"Favourite Books", books, 0, false});
        if (!saved.empty()) rows.push_back({"Favourite Articles", saved, 0, true});
        // Keep focus where it was when the rows are the same.
        for (size_t i = 0; i < rows.size() && i < rows_.size(); ++i)
            if (rows[i].title == rows_[i].title) rows[i].index = std::min(rows_[i].index, (int)rows[i].items.size() - 1);
        rows_ = std::move(rows);
        row_ = std::min(row_, std::max(0, (int)rows_.size() - 1));
    }

    void book_card(gfx::Canvas& c, const Rect& r, const svc::Item& it, bool on) {
        const Theme& t = theme();
        const Rect cover{r.x, r.y, r.w, r.w * 3 / 2};
        draw_cover(c, cover, it.cover, it.title, it.author);
        if (on) draw_focus(c, cover, 10);
        draw_progress(c, {r.x, cover.bottom() + 14, r.w, 6}, it.progress);
        gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 23, r.x, cover.bottom() + 30,
                       gfx::ellipsize(Face::SansSemiBold, 23, it.title, r.w), on ? t.text : t.muted);
    }

    void article_card(gfx::Canvas& c, const Rect& r, const svc::Item& it, bool on) {
        const Theme& t = theme();
        draw_panel(c, r, on);
        if (on) draw_focus(c, r, 14);
        const Rect icon{r.x + 22, r.y + 27, 96, 96};
        c.fill_rounded(icon, 12, gfx::mix(t.surface_hi, gfx::rgb(0xFFFFFF), 20));
        if (const gfx::Image* img = it.cover.empty() ? nullptr : gfx::image_get(it.cover, 64, 64, gfx::Fit::Contain))
            c.draw_image(*img, icon.x + (icon.w - img->lw()) / 2, icon.y + (icon.h - img->lh()) / 2);
        const int tx = icon.right() + 24, tw = r.right() - tx - 22;
        const auto lines = gfx::wrap_text(Face::SansSemiBold, 27, it.title, tw, 2);
        int y = r.y + 24;
        for (const auto& l : lines) {
            gfx::draw_text(c, Face::SansSemiBold, 27, tx, y, l, t.text);
            y += 36;
        }
        gfx::draw_text(c, Face::Sans, 20, tx, r.bottom() - 42, gfx::ellipsize(Face::Sans, 20, it.collection_title, tw), t.muted);
    }

    void options(const svc::Item& item) {
        std::vector<MenuItem> items;
        const svc::Item copy = item;
        items.push_back({"Open", nullptr, [copy] { open_item(copy); }, nullptr});
        items.push_back({item.favourite ? "Remove from favourites" : "Add to favourites", nullptr, [this, copy] {
                             svc::library().set_favourite(copy.id, !copy.favourite);
                             reload();
                         },
                         nullptr});
        if (item.kind == svc::ItemKind::Book)
            items.push_back({"Mark as finished", nullptr, [this, copy] {
                                 if (svc::Item* it = svc::library().find(copy.id)) {
                                     it->finished = true;
                                     svc::library().mark_dirty();
                                 }
                                 reload();
                             },
                             nullptr});
        MenuItem forget{"Remove from history", nullptr, [this, copy] {
                            svc::library().remove(copy.id);
                            reload();
                        },
                        nullptr};
        forget.destructive = true;
        items.push_back(forget);
        open_menu(item.title, std::move(items), item.kind == svc::ItemKind::Article ? item.collection_title : item.author);
    }

    // The welcome screen's steps: what each offers.
    void start_step(int step) {
        std::vector<MenuItem> items;
        if (step == 0) {
            items.push_back({"Browse the Kiwix online library", nullptr, [] {
                                 svc::settings().kiwix_online = true;
                                 svc::settings_save();
                                 app().switch_tab(Tab::Knowledge);
                             },
                             nullptr});
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
            open_menu("Knowledge", std::move(items), "Wikipedia and other collections (ZIM files)");
        } else if (step == 1) {
            items.push_back({"Add a catalogue (Project Gutenberg\xE2\x80\xA6)", nullptr,
                             [] { open_add_catalogue_menu([] { app().switch_tab(Tab::Books); }); }, nullptr});
            items.push_back({"Set up my Calibre-Web server", nullptr, [] { app().switch_tab(Tab::Settings); }, nullptr});
            open_menu("Books", std::move(items), "Online catalogues need no server of your own");
        } else if (step == 2) {
            items.push_back({"Free audiobooks from LibriVox", nullptr, [] {
                                 svc::settings().librivox = true;
                                 svc::settings_save();
                                 app().switch_tab(Tab::Audiobooks);
                             },
                             nullptr});
            items.push_back({"Set up my Audiobookshelf server", nullptr, [] { app().switch_tab(Tab::Settings); }, nullptr});
            open_menu("Audiobooks", std::move(items), "LibriVox needs no server of your own");
        } else {
            app().switch_tab(Tab::Local);
        }
    }

    void welcome(gfx::Canvas& c) {
        const Theme& t = theme();
        const svc::Settings& s = svc::settings();
        const Rect area{kMarginX, kContentTop, kContentW, kFooterTop - kContentTop};
        int y = area.y + 90;
        const char* title = T("Welcome to Librarian");
        gfx::draw_text(c, Face::SansBold, 56, (kWidth - gfx::text_width(Face::SansBold, 56, title)) / 2, y, title, t.text);
        y += 100;
        const char* line = T("Your libraries, free books and audiobooks, and your own documents, on the big screen.");
        gfx::draw_text(c, Face::Sans, 30, (kWidth - gfx::text_width(Face::Sans, 30, line)) / 2, y, line, t.muted);
        y += 110;
        struct Step { const char* name; bool done; const char* text; };
        const Step steps[] = {
            {"Knowledge", !s.kiwix_url.empty() || s.kiwix_online,
             "Wikipedia and more from your Kiwix Server, or find collections in the Kiwix online library."},
            {"Books", !s.calibre_url.empty() || !s.opds_catalogs.empty(),
             "Free books from Project Gutenberg and other catalogues, or your own Calibre-Web server."},
            {"Audiobooks", svc::abs::configured() || s.librivox,
             "Free audiobooks from LibriVox, or your own Audiobookshelf server."},
            {"Local", true, "Your own books, comics, PDFs and ZIM collections on USB drives or /data/books."},
        };
        const int w = 1200, x = (kWidth - w) / 2;
        for (int i = 0; i < 4; ++i) {
            const Step& st = steps[i];
            const Rect r{x, y, w, 100};
            draw_panel(c, r, i == step_);
            if (i == step_) draw_focus(c, r, 14);
            c.fill_circle(r.x + 60, r.y + 50, 22, st.done ? t.success : t.faint);
            gfx::draw_text(c, Face::SansSemiBold, 30, r.x + 110, r.y + 12, T(st.name), t.text);
            gfx::draw_text(c, Face::Sans, 24, r.x + 110, r.y + 56, gfx::ellipsize(Face::Sans, 24, T(st.text), w - 140), t.muted);
            y += 114;
        }
        y += 20;
        const char* hint = T("Press X on a step to start. OPTIONS opens Settings; L1 and R1 move between sections.");
        gfx::draw_text(c, Face::Sans, 26, (kWidth - gfx::text_width(Face::Sans, 26, hint)) / 2, y, hint, t.muted);
    }

    std::vector<Row> rows_;
    int row_ = 0, step_ = 0;
    uint64_t local_generation_ = 0, listening_generation_ = 0;
};

}  // namespace

std::unique_ptr<Screen> make_home_screen() { return std::make_unique<HomeScreen>(); }

}  // namespace ui
