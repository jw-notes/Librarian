// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The page viewer for comics and PDF: whole page, fit to width, or zoomed;
// pages are rendered in the background, with the neighbours prefetched.
#include <math.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <memory>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/paged.h"
#include "gfx/font.h"
#include "services/library.h"
#include "ui/app.h"
#include "ui/overlays.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {
void open_pdf_reader(const svc::Item& item, std::shared_ptr<doc::PagedDocument> document);

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

enum class FitMode { Page = 0, Width = 1 };

class PageViewer : public Screen {
public:
    PageViewer(svc::Item item, std::shared_ptr<doc::PagedDocument> document)
        : item_(std::move(item)), doc_(std::move(document)) {
        page_ = std::max(0, std::min(item_.position.section, doc_->page_count() - 1));
        // Comics read best a page at a time; PDFs (usually text) at full width.
        fit_ = doc_->is_pdf() ? FitMode::Width : FitMode::Page;
        request(page_);
    }

    ~PageViewer() override { save(); }

    bool full_screen() const override { return true; }
    bool animating() const override { return pending_ > 0 || scroll_ != target_scroll_; }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            save();
            app().pop();
            return;
        }
        if (in.press(kTriangle)) menu();
        if (in.press(kSquare)) {
            fit_ = fit_ == FitMode::Page ? FitMode::Width : FitMode::Page;
            zoom_ = 100;
            reset_scroll(false);
            request(page_);
        }
        if (in.repeat(kR2) && zoom_ < 300) set_zoom(zoom_ + 25);
        if (in.repeat(kL2) && zoom_ > 100) set_zoom(zoom_ - 25);
        if (in.repeat(kR1)) go(page_ + 10, false);
        if (in.repeat(kL1)) go(page_ - 10, false);

        const gfx::Image* img = current();
        const int max_y = img ? std::max(0, img->lh() - kHeight) : 0;
        const int max_x = img ? std::max(0, img->lw() - kWidth) : 0;
        const int forward = rtl_ ? kLeft : kRight, back = rtl_ ? kRight : kLeft;
        if (in.repeat(forward)) {
            if (pan_x_ < max_x && zoom_ > 100) pan_x_ = std::min(max_x, pan_x_ + kWidth / 2);
            else go(page_ + 1, false);
        }
        if (in.repeat(back)) {
            if (pan_x_ > 0 && zoom_ > 100) pan_x_ = std::max(0, pan_x_ - kWidth / 2);
            else go(page_ - 1, true);
        }
        if (in.repeat(kDown)) {
            if (target_scroll_ < max_y) target_scroll_ = std::min(max_y, target_scroll_ + kHeight * 2 / 5);
            else if (in.press(kDown)) go(page_ + 1, false);
        }
        if (in.repeat(kUp)) {
            if (target_scroll_ > 0) target_scroll_ = std::max(0, target_scroll_ - kHeight * 2 / 5);
            else if (in.press(kUp)) go(page_ - 1, true);
        }
        if (in.ry) target_scroll_ = std::max(0, std::min(max_y, target_scroll_ + in.ry * std::abs(in.ry) / 250));
        if (in.rx) pan_x_ = std::max(0, std::min(max_x, pan_x_ + in.rx * std::abs(in.rx) / 250));
        const int diff = target_scroll_ - scroll_;
        scroll_ += std::abs(diff) <= 2 ? diff : diff * 2 / 5;
        if (in.pressed) show_bar_until_ = now_ms() + 2500;
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        c.clear(gfx::rgb(0x0B0D11));
        const gfx::Image* img = current();
        if (img) {
            const int x = img->lw() <= kWidth ? (kWidth - img->lw()) / 2 : -pan_x_;
            const int y = img->lh() <= kHeight ? (kHeight - img->lh()) / 2 : -scroll_;
            c.draw_image(*img, x, y);
        } else if (!error_.empty()) {
            draw_message(c, {0, 0, kWidth, kHeight}, "Could not show this page", error_, true);
        } else {
            draw_loading(c, {0, 0, kWidth, kHeight}, "Page " + std::to_string(page_ + 1));
        }
        // The page number fades in after a button press.
        if (now_ms() < show_bar_until_ || !img) {
            std::string label = std::to_string(page_ + 1) + " / " + std::to_string(doc_->page_count());
            if (zoom_ > 100) label += "   \xC2\xB7   " + std::to_string(zoom_) + "%";
            const int w = gfx::text_width(Face::SansSemiBold, 26, label) + 56;
            const Rect r{(kWidth - w) / 2, kHeight - 92, w, 56};
            c.fill_rounded(r, 28, gfx::with_alpha(t.surface, 225));
            gfx::draw_text(c, Face::SansSemiBold, 26, r.x + 28, r.y + 10, label, t.text);
        }
    }

private:
    struct Rendered {
        gfx::Image image;
        int w = 0, h = 0;  // the box it was rendered for
    };

    // The box a page is rendered into for the current mode and zoom.
    void target_box(int& w, int& h) const {
        if (fit_ == FitMode::Page) {
            w = kWidth * zoom_ / 100;
            h = kHeight * zoom_ / 100;
        } else {
            w = std::min(kWidth, 1500) * zoom_ / 100;
            h = 12000;  // effectively unbounded: the width decides
        }
    }

    const gfx::Image* current() const {
        auto it = pages_.find(page_);
        return it == pages_.end() || it->second.image.empty() ? nullptr : &it->second.image;
    }

    void request(int page) {
        int w, h;
        target_box(w, h);
        // The page itself first, then its neighbours.
        for (int p : {page, page + 1, page - 1}) {
            if (p < 0 || p >= doc_->page_count()) continue;
            auto it = pages_.find(p);
            if (it != pages_.end() && it->second.w == w && it->second.h == h) continue;
            if (in_flight_.count(p) && in_flight_[p] == w * 100000 + h) continue;
            in_flight_[p] = w * 100000 + h;
            ++pending_;
            struct Result {
                Rendered page;
                std::string error;
                bool ok = false;
            };
            auto document = doc_;
            lib::async<Result>(
                token_,
                [document, p, w, h]() {
                    Result r;
                    r.page.w = w;
                    r.page.h = h;
                    // At the screen's real resolution (gfx/canvas.h).
                    const float s = gfx::render_scale();
                    r.ok = document->render(p, (int)lroundf(w * s), (int)lroundf(h * s), r.page.image, r.error);
                    r.page.image.density = s;
                    return r;
                },
                [this, p](Result& r) {
                    --pending_;
                    in_flight_.erase(p);
                    if (r.ok) pages_[p] = std::move(r.page);
                    else if (p == page_) error_ = r.error;
                    if (r.ok && p == page_ && land_at_end_) {
                        land_at_end_ = false;
                        reset_scroll(true);
                    }
                    // Keep three pages around the current one.
                    for (auto it = pages_.begin(); it != pages_.end();)
                        it = std::abs(it->first - page_) > 1 ? pages_.erase(it) : std::next(it);
                },
                p == page ? 2 : 1);
        }
    }

    void go(int page, bool from_end) {
        page = std::max(0, std::min(page, doc_->page_count() - 1));
        if (page == page_) {
            if (page == doc_->page_count() - 1 && !from_end) app().toast("Last page");
            return;
        }
        page_ = page;
        error_.clear();
        reset_scroll(from_end);
        request(page_);
        save();
    }

    void reset_scroll(bool to_end) {
        pan_x_ = 0;
        scroll_ = target_scroll_ = 0;
        if (to_end) {
            // Going backwards in fit-to-width lands at the bottom of the page,
            // now or when it has been rendered.
            const gfx::Image* img = current();
            if (img) scroll_ = target_scroll_ = std::max(0, img->lh() - kHeight);
            land_at_end_ = !img;
        }
    }

    void set_zoom(int zoom) {
        zoom_ = zoom;
        request(page_);
    }

    void save() {
        svc::Position p;
        p.section = page_;
        const int n = doc_->page_count();
        svc::library().set_position(item_.id, p, n > 1 ? (float)page_ / (n - 1) : 1.f);
        if (page_ == n - 1) {
            if (svc::Item* it = svc::library().find(item_.id)) it->finished = true;
        }
    }

    void menu() {
        std::vector<MenuItem> items;
        items.push_back({"Go to page", [this] { return std::to_string(page_ + 1); }, nullptr,
                         [this](int d) { go(page_ + d, false); }, true});
        items.push_back({"View", [this] { return std::string(fit_ == FitMode::Page ? "Whole page" : "Fit width"); }, nullptr,
                         [this](int) {
                             fit_ = fit_ == FitMode::Page ? FitMode::Width : FitMode::Page;
                             reset_scroll(false);
                             request(page_);
                         }});
        items.push_back({"Reading direction", [this] { return std::string(rtl_ ? "Right to left" : "Left to right"); },
                         nullptr, [this](int) { rtl_ = !rtl_; }});
        const auto outline = doc_->outline();
        if (!outline.empty())
            items.push_back({"Contents", nullptr, [this, outline] {
                                 std::vector<MenuItem> entries;
                                 for (const auto& o : outline)
                                     entries.push_back({std::string(std::max(0, o.level - 1) * 3, ' ') + o.title,
                                                        [o] { return std::to_string(o.page + 1); },
                                                        [this, o] { go(o.page, false); }, nullptr});
                                 open_menu("Contents", std::move(entries));
                             },
                             nullptr});
        items.push_back({"Bookmark this page", nullptr, [this] {
                             svc::Bookmark b;
                             b.at.section = page_;
                             b.label = "Page " + std::to_string(page_ + 1);
                             b.created = time(nullptr);
                             svc::library().add_bookmark(item_.id, b);
                             app().toast("Bookmark added");
                         },
                         nullptr});
        if (const svc::Item* it = svc::library().find(item_.id); it && !it->bookmarks.empty()) {
            const std::string id = item_.id;
            items.push_back({"Bookmarks", [id] {
                                 const svc::Item* item = svc::library().find(id);
                                 return std::to_string(item ? item->bookmarks.size() : 0);
                             },
                             [this] {
                                 std::vector<MenuItem> marks;
                                 const svc::Item* item = svc::library().find(item_.id);
                                 for (const auto& b : item->bookmarks)
                                     marks.push_back({b.label, nullptr, [this, b] { go(b.at.section, false); }, nullptr});
                                 open_menu("Bookmarks", std::move(marks));
                             },
                             nullptr});
        }
        items.push_back({"Close", nullptr, [this] {
                             save();
                             app().pop();
                         },
                         nullptr});
        open_menu(item_.title, std::move(items), "L2 / R2 zoom, the right stick pans, Square switches the view.");
    }

    svc::Item item_;
    std::shared_ptr<doc::PagedDocument> doc_;
    std::map<int, Rendered> pages_;
    std::map<int, int> in_flight_;
    int page_ = 0, pending_ = 0, zoom_ = 100;
    int scroll_ = 0, target_scroll_ = 0, pan_x_ = 0;
    FitMode fit_ = FitMode::Page;
    bool rtl_ = false, land_at_end_ = false;
    std::string error_;
    uint64_t show_bar_until_ = 0;
};

}  // namespace

void open_paged_document(const svc::Item& item) {
    std::string error;
    auto document = doc::open_paged(item.path, error);
    if (!document) {
        app().toast(error);
        return;
    }
    if (svc::Item* it = svc::library().find(item.id)) {
        if (it->author.empty()) it->author = document->author();
        if (it->cover.empty()) it->cover = "cover:" + item.path;
        svc::library().mark_dirty();
    }
    if (document->is_pdf()) open_pdf_reader(item, document);
    else app().push(std::make_unique<PageViewer>(item, document));
}

}  // namespace ui
