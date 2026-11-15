// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The Local tab: every document Librarian has found on the console and on USB
// drives (see services/local_library.h), as a shelf of covers with filters,
// search and sorting. It follows the library as drives come and go.
#include <algorithm>
#include <cstdio>
#include <map>

#include "core/fs.h"
#include "core/text.h"
#include "doc/archive.h"
#include "doc/covers.h"
#include "gfx/font.h"
#include "gfx/image_cache.h"
#include "services/library.h"
#include "core/worker.h"
#include "services/calibre_session.h"
#include "services/calibre.h"
#include "services/listening.h"
#include "services/local_library.h"
#include "ui/app.h"
#include "ui/i18n.h"
#include "ui/openers.h"
#include "ui/reader.h"
#include "ui/overlays.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

void open_audiobook(const svc::abs::Book& book);  // audiobooks_screens.cpp

namespace {

// Books includes comics; Audiobooks lists the downloaded audiobooks (on the
// console and USB drives), which are not documents.
// Books: ebooks and comics. Docs: documents (PDF, Word, OpenDocument, RTF,
// text, Markdown, HTML) and tables (CSV, TSV).
enum class Filter { All, Books, Audiobooks, Docs, Collections, Downloaded, Usb, Count };
const char* kFilterNames[] = {"All", "Books", "Audiobooks", "Docs", "Collections (ZIM)", "Downloaded", "USB"};

bool is_doc(const svc::LocalDoc& d) {
    if (d.kind == svc::FileKind::Pdf || d.kind == svc::FileKind::Text || d.kind == svc::FileKind::Table) return true;
    static const char* kDocs[] = {"docx", "odt", "rtf", "html", "htm", "xhtml", "md", "markdown", "txt"};
    for (const char* f : kDocs)
        if (d.format == f) return true;
    return false;
}
enum class Sort { Recent, Title, Author };
const char* kSortNames[] = {"Recently read", "Title", "Author"};

class LocalScreen : public Screen {
public:
    LocalScreen() { rebuild(); }

    Tab tab() const override { return Tab::Local; }
    bool animating() const override { return svc::local_library().scanning(); }
    void resumed() override {
        if (audio()) svc::listening().refresh_downloads();
        rebuild();
    }

    std::vector<Hint> hints() const override {
        std::vector<Hint> h = {{kOptions, "Sort & refresh"}, {kSquare, "Search"}, {kL2, "Filter"}};
        if (count() > 0) {
            if (!audio()) h.insert(h.begin(), {kTriangle, "Options"});
            h.insert(h.begin(), {kCross, "Open"});
        }
        return h;
    }

    void update(const Input& in) override {
        // Drives connected or removed, metadata read: the shelf follows.
        if (seen_ != svc::local_library().generation()) rebuild();
        // Downloads found (refresh_downloads answers later) or progress saved.
        if (audio() && seen_audio_ != svc::listening().generation()) rebuild();
        if (in.press(kR2) || in.press(kL2)) {
            const int n = (int)Filter::Count;
            filter_ = (Filter)(((int)filter_ + (in.press(kR2) ? 1 : n - 1)) % n);
            nav_ = GridNav();
            if (audio()) svc::listening().refresh_downloads();
            rebuild();
        }
        if (in.press(kSquare)) search();
        if (in.press(kOptions)) sort_menu();
        if (count() == 0) {
            if (in.press(kCircle) && !query_.empty()) {
                query_.clear();
                rebuild();
            }
            return;
        }
        const Geom g = geometry();
        nav_.update(in, count(), g.cols, g.rows);
        if (in.press(kCircle) && !query_.empty()) {
            query_.clear();
            rebuild();
            return;
        }
        if (audio() && nav_.index < (int)audio_.size()) {
            if (in.press(kCross)) open_audiobook(audio_[nav_.index].book);
            return;
        }
        const svc::LocalDoc& d = view_[nav_.index - offset()];
        if (in.press(kCross)) {
            // A talking book chosen among the audiobooks starts reading aloud.
            if (audio()) request_read_aloud();
            open(d);
        }
        if (in.press(kTriangle)) options(d);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        gfx::draw_text(c, Face::SansBold, kTitlePx, kMarginX, kContentTop, "Local Library", t.text);
        int x = kMarginX + gfx::text_width(Face::SansBold, kTitlePx, "Local Library") + 50;
        for (int i = 0; i < (int)Filter::Count; ++i) {
            const char* name = T(kFilterNames[i]);
            const int w = gfx::text_width(Face::SansSemiBold, 24, name) + 44;
            const bool on = (int)filter_ == i;
            c.fill_rounded({x, kContentTop + 6, w, 48}, 24, on ? t.accent : t.surface);
            gfx::draw_text(c, Face::SansSemiBold, 24, x + 22, kContentTop + 15, name, on ? t.accent_text : t.muted);
            x += w + 12;
        }
        // Status: drives, scanning, search.
        std::string status;
        const int drives = svc::local_library().drives();
        status = drives == 0 ? "No USB drive" : drives == 1 ? "1 USB drive" : std::to_string(drives) + " USB drives";
        if (svc::local_library().scanning()) status += "  \xC2\xB7  Looking for documents\xE2\x80\xA6";
        if (!query_.empty()) status = "\xE2\x80\x9C" + query_ + "\xE2\x80\x9D  \xC2\xB7  " + std::to_string(count()) + " found  \xC2\xB7  " + status;
        const int sw = gfx::text_width(Face::Sans, 22, status);
        gfx::draw_text(c, Face::Sans, 22, kWidth - kMarginX - sw, kContentTop + 18, status, t.muted);
        if (svc::local_library().scanning()) draw_spinner(c, kWidth - kMarginX - sw - 26, kContentTop + 32, 10);

        const Rect area{kMarginX, kContentTop + 80, kContentW, kFooterTop - kContentTop - 80};
        if (count() == 0) {
            if (audio() && query_.empty()) {
                draw_message(c, area, "No downloaded audiobooks",
                             "Audiobooks you download in the Audiobooks section (to the console or a USB drive) appear "
                             "here and play without a connection to the server.");
            } else if (svc::local_library().scanning() && total_ == 0 && !audio()) {
                draw_loading(c, area, "Looking for documents\xE2\x80\xA6");
            } else if (!query_.empty()) {
                draw_message(c, area, "Nothing matches", "No document's title, author or file name contains \xE2\x80\x9C" + query_ +
                                                             "\xE2\x80\x9D. Press Circle to clear the search.");
            } else if (total_ == 0 && !audio()) {
                draw_message(c, area, "No documents yet",
                             "Plug in a USB drive (FAT32 or exFAT) with EPUB, PDF, MOBI, AZW3, FB2, DOCX, ODT, RTF, CBZ, CBR, CB7 "
                             "or TXT files and "
                             "they appear here by themselves. You can also download books in the Books section, or copy "
                             "files to /data/books over FTP.");
            } else {
                draw_message(c, area, "Nothing here",
                             filter_ == Filter::Docs
                                 ? "No documents yet. PDF, Word (DOCX), OpenDocument (ODT), RTF, text (TXT), Markdown (MD) "
                                   "and HTML files on USB drives and in /data/books appear here."
                                 : "No documents match this filter.");
            }
            return;
        }
        // Height-first grid derived to fit between the chips and the footer.
        const Geom g = geometry();
        const int cols = g.cols, visible_rows = g.rows, card_w = g.card_w, cover_h = g.cover_h, row_h = g.row_h;
        nav_.clamp(count(), cols, visible_rows);
        for (int i = nav_.top_row * cols; i < std::min(count(), (nav_.top_row + visible_rows) * cols); ++i) {
            const int row = i / cols - nav_.top_row, col = i % cols;
            const Card k = card(i);
            const Rect cover{g.left + col * (card_w + g.gap), area.y + row * row_h, card_w, cover_h};
            const bool on = i == nav_.index;
            draw_cover(c, cover, k.image, k.title, k.author);
            if (!k.available) {
                c.fill_rounded(cover, 10, gfx::with_alpha(gfx::rgb(0x0B0E14), 150));
                const char* label = "Not connected";
                const int lw = gfx::text_width(Face::SansSemiBold, 20, label) + 24;
                c.fill_rounded({cover.x + (cover.w - lw) / 2, cover.y + cover.h / 2 - 20, lw, 40}, 20, t.surface_hi);
                gfx::draw_text(c, Face::SansSemiBold, 20, cover.x + (cover.w - lw) / 2 + 12, cover.y + cover.h / 2 - 13, label, t.muted);
            }
            std::string badge = k.badge;
            for (char& ch : badge) ch = (char)toupper((unsigned char)ch);
            const int bw = gfx::text_width(Face::SansBold, 17, badge) + 18;
            c.fill_rounded({cover.right() - bw - 8, cover.y + 8, bw, 30}, 8, gfx::with_alpha(gfx::rgb(0x0B0E14), 210));
            gfx::draw_text(c, Face::SansBold, 17, cover.right() - bw + 1, cover.y + 12, badge, gfx::rgb(0xE6EAF2));
            if (on) draw_focus(c, cover, 10);
            int y = cover.bottom() + 12;
            if (k.progress > 0) {
                draw_progress(c, {cover.x, y, cover.w, 6}, k.progress);
                y += 14;
            }
            gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 22, cover.x, y,
                           gfx::ellipsize(Face::SansSemiBold, 22, k.title, cover.w), on ? t.text : t.muted);
            gfx::draw_text(c, Face::Sans, 18, cover.x, y + 32, gfx::ellipsize(Face::Sans, 18, k.place, cover.w), t.faint);
        }
        const int total_rows = (count() + cols - 1) / cols;
        draw_scrollbar(c, {kWidth - kMarginX + 30, area.y, 8, area.h - 20}, nav_.top_row, visible_rows, total_rows);
        // Where the focused document is.
        const Card k = card(nav_.index);
        const std::string where = k.where + "   \xC2\xB7   " + lib::format_bytes(k.size) + "   \xC2\xB7   " +
                                  std::to_string(nav_.index + 1) + " of " + std::to_string(count());
        gfx::draw_text(c, Face::Sans, 22, kMarginX, kFooterTop + 26, gfx::ellipsize(Face::Sans, 22, where, 1000), t.faint);
    }

private:
    // Upper bounds; the real grid is derived to fit (see geometry()).
    static constexpr int kMaxCols = 9, kMaxRows = 2;

    struct Geom { int cols, rows, card_w, cover_h, gap, left, row_h, label_h; };
    static Geom geometry() {
        const Rect area{kMarginX, kContentTop + 80, kContentW, kFooterTop - kContentTop - 80};
        Geom g;
        g.gap = 30;
        g.label_h = 90;
        g.rows = kMaxRows;
        g.cover_h = std::min(300, (area.h - (g.rows - 1) * g.gap) / g.rows - g.label_h);
        g.card_w = g.cover_h * 2 / 3;
        g.cols = std::max(1, std::min(kMaxCols, (kContentW + g.gap) / (g.card_w + g.gap)));
        g.row_h = g.cover_h + g.label_h;
        g.rows = std::max(1, std::min(kMaxRows, area.h / g.row_h));
        const int grid_w = g.cols * g.card_w + (g.cols - 1) * g.gap;
        g.left = area.x + (kContentW - grid_w) / 2;
        return g;
    }

    bool audio() const { return filter_ == Filter::Audiobooks; }
    // Under Audiobooks: the audiobooks first, then talking books (documents).
    int offset() const { return audio() ? (int)audio_.size() : 0; }
    int count() const { return offset() + (int)view_.size(); }

    // What a grid card shows, for a document or a downloaded audiobook.
    struct Card {
        std::string image, title, author, badge, place, where;
        float progress = 0;
        uint64_t size = 0;
        bool available = true;
    };
    Card card(int i) const {
        Card k;
        if (audio() && i < (int)audio_.size()) {
            const svc::AudiobookDownload& a = audio_[i];
            const svc::LocalProgress p = svc::listening().progress(a.item_id);
            k.image = !a.cover.empty() ? a.cover : doc::find_cover_key(a.book.title, a.book.author);
            k.title = a.book.title;
            k.author = a.book.author;
            k.badge = "Audio";
            k.place = k.where = a.on_usb ? "USB" : "Internal";
            k.progress = p.finished ? 1.f : p.duration > 0 ? (float)(p.time / p.duration) : 0.f;
            k.size = a.bytes;
            return k;
        }
        const svc::LocalDoc& d = view_[i - offset()];
        const svc::Item* item = svc::library().find_file(d.path, d.fingerprint);
        // Every book may have a cover: its own, a picture beside it, or one
        // found online (doc/covers.cpp); tables and collections have none.
        const bool has_cover = d.available && d.kind != svc::FileKind::Table;
        k.image = !d.available ? "" : item && !item->cover.empty() ? item->cover : has_cover ? doc::cover_key(d.path) : "";
        k.title = display_title(d, item);
        k.author = item && !item->author.empty() ? item->author : d.author;
        k.badge = d.format;
        k.place = d.place();
        k.where = d.place() + (d.folder.empty() ? "" : "  \xE2\x80\xBA  " + d.folder);
        k.progress = item ? item->progress : 0.f;
        k.size = d.size;
        k.available = d.available;
        return k;
    }

    static std::string display_title(const svc::LocalDoc& d, const svc::Item* item) {
        // A title from the server (Calibre) or the reader beats one from the file name.
        if (item && !item->title.empty() && (d.title.empty() || item->title != lib::file_stem(d.path))) return item->title;
        return d.title;
    }

    void rebuild() {
        seen_ = svc::local_library().generation();
        seen_audio_ = svc::listening().generation();
        const auto& all = svc::local_library().docs();
        // One entry per book: the same file in several places (a downloaded copy
        // and one on USB) shows once, from the most dependable place.
        std::map<std::string, const svc::LocalDoc*> best;
        for (const svc::LocalDoc& d : all) {
            const svc::LocalDoc*& b = best[d.fingerprint];
            if (!b || (d.available && !b->available) || (d.available == b->available && d.origin < b->origin)) b = &d;
        }
        total_ = best.size();
        const std::string q = lib::lower(query_);
        audio_.clear();
        if (audio()) {
            for (const svc::AudiobookDownload& a : svc::listening().downloads())
                if (q.empty() || lib::lower(a.book.title + " " + a.book.author).find(q) != std::string::npos)
                    audio_.push_back(a);
            std::stable_sort(audio_.begin(), audio_.end(), [&](const svc::AudiobookDownload& a, const svc::AudiobookDownload& b) {
                if (sort_ == Sort::Recent) {
                    const int64_t ua = svc::listening().progress(a.item_id).updated, ub = svc::listening().progress(b.item_id).updated;
                    if (ua != ub) return ua > ub;
                } else if (sort_ == Sort::Author && !lib::iequals(a.book.author, b.book.author)) {
                    return lib::natural_less(a.book.author, b.book.author);
                }
                return lib::natural_less(a.book.title, b.book.title);
            });
        }
        view_.clear();
        for (const auto& [fp, d] : best) {
            const bool keep = filter_ == Filter::All || (filter_ == Filter::Audiobooks && d->narrated && d->available) ||
                              (filter_ == Filter::Books && !is_doc(*d) &&
                               (d->kind == svc::FileKind::Book || d->kind == svc::FileKind::Comic)) ||
                              (filter_ == Filter::Docs && is_doc(*d)) ||
                              (filter_ == Filter::Collections && d->kind == svc::FileKind::Collection) ||
                              (filter_ == Filter::Downloaded && d->origin == svc::Origin::Downloaded) ||
                              (filter_ == Filter::Usb && d->origin == svc::Origin::Usb);
            if (!keep) continue;
            if (!q.empty()) {
                const svc::Item* it = svc::library().find_file(d->path, d->fingerprint);
                const std::string text = lib::lower(d->title + " " + d->author + " " + lib::file_name(d->path) + " " +
                                                    (it ? it->title + " " + it->author : std::string()));
                if (text.find(q) == std::string::npos) continue;
            }
            view_.push_back(*d);
        }
        auto opened = [](const svc::LocalDoc& d) {
            const svc::Item* it = svc::library().find_file(d.path, d.fingerprint);
            return it ? it->opened : 0;
        };
        auto title = [](const svc::LocalDoc& d) { return display_title(d, svc::library().find_file(d.path, d.fingerprint)); };
        std::stable_sort(view_.begin(), view_.end(), [&](const svc::LocalDoc& a, const svc::LocalDoc& b) {
            if (sort_ == Sort::Recent) {
                const int64_t oa = opened(a), ob = opened(b);
                if (oa != ob) return oa > ob;
            } else if (sort_ == Sort::Author && !lib::iequals(a.author, b.author)) {
                if (a.author.empty() != b.author.empty()) return !a.author.empty();
                return lib::natural_less(a.author, b.author);
            }
            return lib::natural_less(title(a), title(b));
        });
        { const Geom g = geometry(); nav_.clamp(count(), g.cols, g.rows); }
    }

    void search() {
        open_keyboard("Search the Local Library", query_, KeyboardKind::Text, [this](const std::string& q) {
            query_ = lib::trim(q);
            nav_ = GridNav();
            rebuild();
        });
    }

    void sort_menu() {
        std::vector<MenuItem> items;
        items.push_back({"Sort by", [this] { return std::string(kSortNames[(int)sort_]); }, nullptr,
                         [this](int d) {
                             sort_ = (Sort)(((int)sort_ + 3 + d) % 3);
                             rebuild();
                         }});
        items.push_back({"Refresh Library", nullptr, [] {
                             svc::local_library().refresh();
                             app().toast("Looking for documents again\xE2\x80\xA6");
                         },
                         nullptr});
        items.push_back({"Clean missing entries", nullptr, [this] {
                             confirm("Remove books whose files are missing?",
                                     "Books on connected drives whose files no longer exist are taken off the shelf. "
                                     "Disconnected drives are left alone, and no files are deleted.",
                                     "Clean", [this] {
                                         const int n = svc::local_library().clean_missing();
                                         app().toast(n == 0 ? "No missing books found"
                                                            : std::to_string(n) + (n == 1 ? " book removed" : " books removed"));
                                         rebuild();
                                     });
                         },
                         nullptr});
        if (!query_.empty())
            items.push_back({"Clear search", nullptr, [this] {
                                 query_.clear();
                                 rebuild();
                             },
                             nullptr});
        open_menu("Local Library", std::move(items),
                  "Librarian finds documents by itself on USB drives, in /data/books and in its downloads. Refresh looks "
                  "again now.");
    }

    void open(const svc::LocalDoc& d) {
        if (!d.available) {
            app().toast("Connect the USB drive with this book to read it. Your place is kept.");
            return;
        }
        const svc::Item* it = svc::library().find_file(d.path, d.fingerprint);
        open_file(d.path, it ? it->title : d.title, it ? it->author : d.author);
    }

    // A read-only summary of one book.
    void details(const svc::LocalDoc& d) {
        const svc::Item* it = svc::library().find_file(d.path, d.fingerprint);
        std::vector<MenuItem> items;
        auto row = [&](const std::string& label, std::string value) {
            items.push_back({label, [value] { return value; }, [] {}, nullptr});
        };
        const std::string author = it && !it->author.empty() ? it->author : d.author;
        row("Format", d.format.empty() ? "\xE2\x80\x94" : d.format);
        if (!author.empty()) row("Author", author);
        row("Location", d.place() + (d.folder.empty() ? "" : " \xE2\x80\xBA " + d.folder));
        row("Size", lib::format_bytes(d.size));
        row("Status", d.available ? "On a connected drive" : "Drive not connected");
        if (it && it->opened) {
            char pct[16];
            snprintf(pct, sizeof pct, "%d%%", (int)(it->progress * 100 + 0.5f));
            row("Progress", it->finished ? "Finished" : pct);
            if (!it->bookmarks.empty()) row("Bookmarks", std::to_string(it->bookmarks.size()));
        }
        open_menu(it && !it->title.empty() ? it->title : d.title, std::move(items), "Book details");
    }

    // Deletes the file itself; `clear_history` also forgets its bookmarks and position.
    void delete_file(const svc::LocalDoc& doc, bool clear_history) {
        doc::close_archive(doc.path);
        gfx::images_forget(doc::cover_key(doc.path));
        if (lib::remove_file(doc.path)) {
            if (const svc::Item* it = svc::library().find_file(doc.path, doc.fingerprint))
                if (clear_history) svc::library().remove(it->id);
            svc::local_library().remove_entry(doc.fingerprint);
            app().toast(clear_history ? "Deleted" : "Deleted (reading history kept)");
            rebuild();
        } else {
            app().toast("The file could not be deleted (the drive may be read-only).");
        }
    }

    void options(const svc::LocalDoc& d) {
        std::vector<MenuItem> items;
        const svc::LocalDoc doc = d;
        const svc::Item* it0 = svc::library().find_file(d.path, d.fingerprint);
        const bool in_progress = it0 && it0->opened && !it0->finished && it0->progress > 0.0f;
        if (d.available)
            items.push_back({in_progress ? "Continue reading" : "Open", nullptr, [this, doc] { open(doc); }, nullptr});
        items.push_back({"View book details", nullptr, [this, doc] { details(doc); }, nullptr});
        if (const svc::Item* it = svc::library().find_file(d.path, d.fingerprint)) {
            const std::string id = it->id;
            items.push_back({it->favourite ? "Remove from favourites" : "Add to favourites", nullptr, [this, id] {
                                 const svc::Item* item = svc::library().find(id);
                                 svc::library().set_favourite(id, !(item && item->favourite));
                                 rebuild();
                             },
                             nullptr});
            items.push_back({"Forget reading position", nullptr, [this, id] {
                                 if (svc::Item* item = svc::library().find(id)) {
                                     item->position = svc::Position();
                                     item->progress = 0;
                                     item->finished = false;
                                     svc::library().mark_dirty();
                                 }
                                 rebuild();
                             },
                             nullptr});
        }
        if (d.available && !svc::calibre::server().empty()) {
            const std::string path = doc.path, title = doc.title;
            items.push_back({"Upload to Calibre-Web", nullptr, [path, title] {
                                 app().toast("Uploading " + title + " to Calibre-Web…");
                                 lib::async<std::pair<bool, std::string>>(
                                     nullptr,
                                     [path]() {
                                         std::string m;
                                         const bool ok = svc::calibre::upload_book(path, m);
                                         return std::make_pair(ok, m);
                                     },
                                     [](std::pair<bool, std::string>& r) { app().toast(r.second); });
                             },
                             nullptr});
        }
        // Remove from Library takes the book off the shelf but keeps the file;
        // it is the way to clear a stale entry for a file that is already gone.
        {
            MenuItem rem{"Remove from Library", nullptr, [this, doc] {
                             confirm("Remove \xE2\x80\x9C" + doc.title + "\xE2\x80\x9D from your Library?",
                                     "This takes it off the shelf only. The file is not deleted from " + doc.place() +
                                     ", and its reading history is kept in case the file is found again.",
                                     "Remove", [this, doc] {
                                         svc::local_library().remove_entry(doc.fingerprint);
                                         app().toast("Removed from Library");
                                         rebuild();
                                     });
                         },
                         nullptr};
            rem.destructive = true;
            items.push_back(rem);
        }
        // Deleting the file is separate and offers an explicit history choice.
        if (d.available) {
            MenuItem del{"Delete file\xE2\x80\xA6", nullptr, [this, doc] {
                             std::vector<MenuItem> choices;
                             MenuItem keep{"Delete file, keep reading history", nullptr,
                                           [this, doc] { delete_file(doc, false); }, nullptr};
                             keep.destructive = true;
                             choices.push_back(keep);
                             MenuItem both{"Delete file and reading history", nullptr,
                                           [this, doc] { delete_file(doc, true); }, nullptr};
                             both.destructive = true;
                             choices.push_back(both);
                             open_menu("Delete \xE2\x80\x9C" + doc.title + "\xE2\x80\x9D?", std::move(choices),
                                       "The file is removed from " + doc.place() +
                                       ". It stays on your Calibre-Web server. Press O to cancel.");
                         },
                         nullptr};
            del.destructive = true;
            items.push_back(del);
        }
        std::string where = d.place() + (d.folder.empty() ? "" : " \xE2\x80\xBA " + d.folder) + "  \xC2\xB7  " +
                            lib::format_bytes(d.size);
        if (!d.available) where += "  \xC2\xB7  not connected";
        open_menu(d.title, std::move(items), where);
    }

    std::vector<svc::LocalDoc> view_;
    std::vector<svc::AudiobookDownload> audio_;  // the Audiobooks filter
    size_t total_ = 0;
    uint64_t seen_ = (uint64_t)-1, seen_audio_ = (uint64_t)-1;
    Filter filter_ = Filter::All;
    Sort sort_ = Sort::Recent;
    std::string query_;
    GridNav nav_;
};

}  // namespace

std::unique_ptr<Screen> make_local_screen() { return std::make_unique<LocalScreen>(); }

}  // namespace ui
