// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// PDF-specific controls over the existing PagedDocument/MuPDF backend.
// One outstanding tile job and an LRU byte budget keep zoom/pan memory bounded.
#include <math.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>
#include <time.h>
#include "core/worker.h"
#include "doc/paged.h"
#include "gfx/font.h"
#include "services/library.h"
#include "services/settings.h"
#include "ui/app.h"
#include "ui/overlays.h"
#include "ui/pdf_view.h"
#include "ui/pointer.h"
#include "ui/widgets.h"

namespace ui {
namespace {
using namespace platform;
constexpr int kTile = 512, kGap = 24;
constexpr gfx::Rect kViewport{40, 32, 1840, 1016};
constexpr size_t kCacheBytes = 48 * 1024 * 1024;
using TileKey = std::tuple<int, int, int, int, int>;
struct Metadata { doc::PageInfo info; std::vector<doc::PageLink> links; };
struct Tile { gfx::Image image; uint64_t used = 0; };
struct Frame { int page, x, y; PdfView view; };
struct Hit { int page = -1, link = -1; };

class PdfReader : public Screen {
public:
    PdfReader(svc::Item item, std::shared_ptr<doc::PagedDocument> doc) : item_(std::move(item)), doc_(std::move(doc)) {
        const auto& s = svc::settings();
        view_.fit = s.pdf_fit; view_.zoom = s.pdf_zoom; view_.rotation = s.pdf_rotation;
        page_ = std::clamp(item_.position.section, 0, doc_->page_count() - 1);
        resume_ = item_.position; restore_ = true;
        outline_ = doc_->outline();
        bar_until_ = now_ms() + 5000;
        schedule();
    }
    ~PdfReader() override { save(true); }
    bool full_screen() const override { return true; }
    bool animating() const override { return busy_ || pointer_.shown() || now_ms() < bar_until_; }
    void resumed() override { pointer_.hide(); last_tick_ = 0; bar_until_ = now_ms() + 3000; }

    void update(const Input& in) override {
        const uint64_t now = now_ms();
        const float dt = last_tick_ ? std::min<uint64_t>(50, now - last_tick_) / 1000.f : 1.f / 60;
        last_tick_ = now;
        if (in.press(kCircle)) {
            if (!history_.empty()) {
                const auto h = history_.back(); history_.pop_back();
                page_ = h.page; view_ = h.view; restore_ = false; destination_ = false;
                changed();
                save(true);
            } else { save(true); app().pop(); }
            return;
        }
        if (in.press(kOptions)) { pointer_.hide(); settings_menu(); return; }
        if (in.press(kTriangle)) { pointer_.hide(); navigation(); return; }
        if (in.press(kSquare)) bookmark();
        if (in.press(kR3)) { pan_mode_ = !pan_mode_; pointer_.hide(); }
        if (in.repeat(kL2)) zoom(-10);
        if (in.repeat(kR2)) zoom(10);
        if (in.repeat(kR1)) go(page_ + 10);
        if (in.repeat(kL1)) go(page_ - 10);
        // The left stick scrolls; physical D-pad Left/Right always turns pages.
        if ((in.digital & kRight) && in.repeat(kRight)) go(page_ + 1);
        if ((in.digital & kLeft) && in.repeat(kLeft)) go(page_ - 1);
        float dx = in.lx / 127.f * 1000 * dt, dy = in.ly / 127.f * 1000 * dt;
        if ((in.digital & kDown) && in.repeat(kDown)) dy += kViewport.h * .4f;
        if ((in.digital & kUp) && in.repeat(kUp)) dy -= kViewport.h * .4f;
        if (pan_mode_) {
            dx += in.rx / 127.f * 1300 * dt; dy += in.ry / 127.f * 1300 * dt;
        } else if (pointer_.update(in.rx, in.ry, now, svc::settings().cursor_speed)) request_redraw();
        const bool moving = dx != 0 || dy != 0 || in.down(kL2 | kR2 | kLeft | kRight | kUp | kDown);
        if (dx || dy) scroll(dx, dy);
        if (in.press(kCross) && !moving && !pan_mode_) activate(hovered());
        if (in.pressed || in.rx || in.ry || in.lx || in.ly) bar_until_ = now + 3000;
        save(false);
        schedule();
    }

    void draw(gfx::Canvas& c) override {
        static const uint32_t backgrounds[] = {0x0B0D11, 0x777777, 0xD9CDB4, 0x000000};
        c.clear(gfx::rgb(backgrounds[svc::settings().pdf_background]));
        const auto frames = visible_frames();
        const auto hit = hovered();
        c.push_clip(kViewport);
        for (const auto& f : frames) {
            const auto size = f.view.size();
            c.fill({f.x, f.y, (int)size.x, (int)size.y}, gfx::rgb(0xFFFFFF));
            const gfx::Rect visible = gfx::Rect{f.x, f.y, (int)size.x, (int)size.y}.intersect(kViewport);
            if (visible.empty()) continue;
            for (int ty = std::max(0, (visible.y - f.y) / kTile); ty <= (visible.bottom() - 1 - f.y) / kTile; ++ty)
                for (int tx = std::max(0, (visible.x - f.x) / kTile); tx <= (visible.right() - 1 - f.x) / kTile; ++tx) {
                    auto tile = tiles_.find(key(f, tx, ty));
                    if (tile != tiles_.end()) {
                        tile->second.used = ++clock_;
                        c.draw_image(tile->second.image, f.x + tx * kTile, f.y + ty * kTile);
                    }
                }
            auto meta = metadata_.find(f.page);
            if (meta == metadata_.end()) continue;
            if (hit.page == f.page && hit.link >= 0) {
                const auto rect = f.view.link_rect(meta->second.links[hit.link], f.x, f.y);
                c.fill(rect, gfx::with_alpha(gfx::rgb(0x398AFF), 65));
                c.stroke_rounded(rect, 3, 2, gfx::rgb(0x3686FF));
            }
        }
        c.pop_clip();
        if (!error_.empty()) draw_message(c, kViewport, "Could not render PDF", error_, true);
        else if (frames.empty()) draw_loading(c, kViewport, "Loading PDF page");
        if (busy_) draw_spinner(c, 1880, 40, 14);
        if (now_ms() < bar_until_ || frames.empty()) {
            c.fill_rounded({240, 934, 1440, 114}, 18, gfx::with_alpha(gfx::rgb(0x18202D), 240));
            const int percent = metadata_.count(page_) ? (int)std::lround(view_.scale() / (96.f / 72) * 100) : view_.zoom;
            const std::string label = "Page " + std::to_string(page_ + 1) + " / " + std::to_string(doc_->page_count()) +
                "   |   " + std::to_string(percent) + "%   |   " + (pan_mode_ ? "Pan mode (R3: cursor)" : "Cursor mode (R3: pan)");
            gfx::draw_text(c, gfx::Face::SansSemiBold, 26, 270, 950, label, gfx::rgb(0xFFFFFF));
            gfx::draw_text(c, gfx::Face::Sans, 22, 270, 993,
                "Left stick: scroll   L2/R2: zoom   Triangle: pages   Square: bookmark   Options: PDF tools", gfx::rgb(0xC4CDDB));
        }
        pointer_.draw(c, hit.link >= 0);
    }
private:
    PdfView for_page(int page) const {
        PdfView v = view_;
        const auto it = metadata_.find(page);
        v.page = it == metadata_.end() ? doc::PageInfo{} : it->second.info;
        return v;
    }
    std::vector<Frame> visible_frames() const {
        std::vector<Frame> frames;
        if (!metadata_.count(page_)) return frames;
        const bool continuous = svc::settings().pdf_continuous;
        auto v = for_page(page_);
        const auto size = v.size();
        int y = kViewport.y - (int)view_.pan_y;
        if (!continuous && size.y < kViewport.h) y += (kViewport.h - (int)size.y) / 2;
        for (int p = page_; p < doc_->page_count() && frames.size() < 32; ++p) {
            if (!metadata_.count(p) || y >= kViewport.bottom()) break;
            v = for_page(p);
            const auto s = v.size();
            const int x = kViewport.x + (s.x <= kViewport.w ? (kViewport.w - (int)s.x) / 2 : -(int)view_.pan_x);
            frames.push_back({p, x, y, v});
            if (!continuous) break;
            y += (int)s.y + kGap;
        }
        return frames;
    }
    TileKey key(const Frame& f, int tx, int ty) const {
        return {f.page, (int)std::lround(f.view.scale() * 10000), f.view.rotation, tx, ty};
    }
    void request_metadata(int p) {
        if (busy_ || p < 0 || p >= doc_->page_count() || metadata_.count(p)) return;
        busy_ = true;
        auto document = doc_;
        lib::async<Metadata>(token_, [document, p] { return Metadata{document->page_info(p), document->links(p)}; },
            [this, p](Metadata& m) {
                busy_ = false;
                if (m.info.width <= 0 || m.info.height <= 0) { error_ = "Could not read the page dimensions."; return; }
                metadata_[p] = std::move(m);
                if (p == page_) {
                    view_.page = metadata_[p].info;
                    if (restore_) {
                        const auto s = view_.size();
                        view_.pan_x = std::clamp(resume_.block / 100000.f, 0.f, 1.f) * std::max(0.f, s.x - kViewport.w);
                        view_.pan_y = std::clamp(resume_.offset, 0.f, 1.f) *
                            (svc::settings().pdf_continuous ? s.y : std::max(0.f, s.y - kViewport.h));
                        restore_ = false;
                    }
                    if (destination_) {
                        const auto pos = view_.project(dest_x_, dest_y_);
                        view_.pan_x = pos.x; view_.pan_y = pos.y; destination_ = false;
                    }
                    view_.clamp(svc::settings().pdf_continuous, page_ == doc_->page_count() - 1);
                }
                for (auto it = metadata_.begin(); it != metadata_.end();)
                    it = std::abs(it->first - page_) > 32 ? metadata_.erase(it) : std::next(it);
                request_redraw();
                schedule();
            }, 2);
    }
    void schedule() {
        if (busy_ || !error_.empty()) return;
        if (!metadata_.count(page_)) { request_metadata(page_); return; }
        view_.page = metadata_[page_].info;
        auto frames = visible_frames();
        // Fill an exposed next page before starting its tiles.
        if (svc::settings().pdf_continuous && !frames.empty()) {
            const auto& last = frames.back();
            if (last.y + last.view.size().y + kGap < kViewport.bottom() && last.page + 1 < doc_->page_count()) {
                request_metadata(last.page + 1); return;
            }
        }
        for (const auto& f : frames) {
            const auto s = f.view.size();
            const auto visible = gfx::Rect{f.x, f.y, (int)s.x, (int)s.y}.intersect(kViewport);
            if (visible.empty()) continue;
            for (int ty = std::max(0, (visible.y - f.y) / kTile); ty <= (visible.bottom() - 1 - f.y) / kTile; ++ty)
                for (int tx = std::max(0, (visible.x - f.x) / kTile); tx <= (visible.right() - 1 - f.x) / kTile; ++tx) {
                    const auto tile_key = key(f, tx, ty);
                    if (tiles_.count(tile_key)) continue;
                    const gfx::Rect region{tx * kTile, ty * kTile, std::min(kTile, (int)s.x - tx * kTile), std::min(kTile, (int)s.y - ty * kTile)};
                    auto document = doc_;
                    const int generation = generation_;
                    busy_ = true;
                    struct Result { gfx::Image image; std::string error; bool ok = false; };
                    lib::async<Result>(token_, [document, f, region] {
                        Result r;
                        // At the screen's real resolution: the tile's edges and the
                        // page scale in real pixels (gfx/canvas.h).
                        const float s = gfx::render_scale();
                        const int x0 = (int)lroundf(region.x * s), y0 = (int)lroundf(region.y * s);
                        const gfx::Rect real{x0, y0, (int)lroundf(region.right() * s) - x0, (int)lroundf(region.bottom() * s) - y0};
                        r.ok = document->render_region(f.page, f.view.scale() * s, f.view.rotation * 90, real, r.image, r.error);
                        r.image.density = s;
                        return r;
                    }, [this, generation, tile_key](Result& r) {
                        busy_ = false;
                        if (generation != generation_) { schedule(); return; }
                        if (!r.ok) { error_ = r.error; return; }
                        bytes_ += r.image.bytes();
                        tiles_[tile_key] = {std::move(r.image), ++clock_};
                        while (bytes_ > kCacheBytes && !tiles_.empty()) {
                            auto oldest = std::min_element(tiles_.begin(), tiles_.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                            bytes_ -= oldest->second.image.bytes(); tiles_.erase(oldest);
                        }
                        request_redraw();
                        schedule();
                    }, 2);
                    return;
                }
        }
        // Cheap metadata prefetch, never a queue of off-screen raster jobs.
        if (page_ > 0 && !metadata_.count(page_ - 1)) request_metadata(page_ - 1);
        else if (page_ + 1 < doc_->page_count() && !metadata_.count(page_ + 1)) request_metadata(page_ + 1);
    }
    Hit hovered() const {
        if (!pointer_.shown() || pan_mode_ || !error_.empty()) return {};
        if (pointer_.x() < kViewport.x || pointer_.x() >= kViewport.right() || pointer_.y() < kViewport.y || pointer_.y() >= kViewport.bottom()) return {};
        if (now_ms() < bar_until_ && pointer_.y() >= 934 && pointer_.x() >= 240 && pointer_.x() < 1680) return {};
        for (const auto& f : visible_frames()) {
            const auto it = metadata_.find(f.page);
            if (it == metadata_.end()) continue;
            const int x = pointer_.x() - f.x, y = pointer_.y() - f.y;
            if (x < 0 || y < 0 || !tiles_.count(key(f, x / kTile, y / kTile))) continue;
            for (size_t i = 0; i < it->second.links.size(); ++i) {
                const auto r = f.view.link_rect(it->second.links[i], f.x, f.y);
                if (pointer_.x() >= r.x && pointer_.x() < r.right() && pointer_.y() >= r.y && pointer_.y() < r.bottom()) return {f.page, (int)i};
            }
        }
        return {};
    }
    void activate(Hit hit) {
        if (hit.link < 0) return;
        const auto link = metadata_[hit.page].links[hit.link];
        if (link.page >= 0 && link.page < doc_->page_count()) {
            history_.push_back({page_, view_});
            go(link.page);
            dest_x_ = link.dest_x; dest_y_ = link.dest_y; destination_ = true;
            if (metadata_.count(page_)) {
                const auto p = view_.project(dest_x_, dest_y_);
                view_.pan_x = p.x; view_.pan_y = p.y; destination_ = false;
                view_.clamp(svc::settings().pdf_continuous, page_ == doc_->page_count() - 1);
            }
            pointer_.hide();
        } else {
            open_menu("External PDF link", {{"Close", nullptr, [] {}, nullptr}},
                      "Web links cannot be opened here: " + link.uri);
        }
    }
    void changed() { ++generation_; error_.clear(); pointer_.hide(); bar_until_ = now_ms() + 3000; schedule(); }
    void go(int page) {
        save(true);
        page_ = std::clamp(page, 0, doc_->page_count() - 1);
        view_.pan_x = view_.pan_y = 0; restore_ = destination_ = false;
        if (metadata_.count(page_)) view_.page = metadata_[page_].info;
        changed();
    }
    void scroll(float dx, float dy) {
        if (!metadata_.count(page_)) return;
        view_.pan_x += dx; view_.pan_y += dy;
        if (svc::settings().pdf_continuous) {
            const float height = view_.size().y + kGap;
            if (view_.pan_y >= height && page_ + 1 < doc_->page_count()) {
                view_.pan_y -= height; ++page_; changed();
                if (metadata_.count(page_)) view_.page = metadata_[page_].info;
            } else if (view_.pan_y < 0 && page_ > 0 && metadata_.count(page_ - 1)) {
                --page_; view_.page = metadata_[page_].info;
                view_.pan_y += view_.size().y + kGap; changed();
            }
        }
        if (metadata_.count(page_)) view_.clamp(svc::settings().pdf_continuous, page_ == doc_->page_count() - 1);
    }
    void preferences() {
        auto& s = svc::settings();
        s.pdf_fit = view_.fit; s.pdf_zoom = view_.zoom; s.pdf_rotation = view_.rotation;
        svc::settings_save();
    }
    void zoom(int delta) {
        const int percent = (int)std::lround(view_.scale() / (96.f / 72) * 100);
        view_.zoom_to(percent + delta); changed(); preferences();
    }
    void fit(int mode) {
        view_.fit = mode; view_.zoom = 100; view_.pan_x = view_.pan_y = 0;
        changed(); preferences();
    }
    void rotate(int delta) {
        const auto before = view_.size();
        const auto center = view_.unproject(view_.pan_x + std::min((float)kViewport.w, before.x) / 2.f, view_.pan_y + std::min((float)kViewport.h, before.y) / 2.f);
        view_.rotation = (view_.rotation + 4 + delta) % 4;
        const auto pos = view_.project(center.x, center.y);
        view_.pan_x = pos.x - kViewport.w / 2.f; view_.pan_y = pos.y - kViewport.h / 2.f;
        view_.clamp(svc::settings().pdf_continuous, page_ == doc_->page_count() - 1);
        changed(); preferences();
    }
    void save(bool force) {
        if (!metadata_.count(page_) || (restore_ && !force)) return;
        const uint64_t now = now_ms();
        if (!force && now - saved_at_ < 1000) return;
        saved_at_ = now;
        svc::Position p; p.section = page_;
        const auto s = view_.size();
        const float max_x = std::max(0.f, s.x - kViewport.w);
        const float max_y = svc::settings().pdf_continuous ? s.y : std::max(0.f, s.y - kViewport.h);
        p.block = max_x ? (int)(100000 * view_.pan_x / max_x) : 0;
        p.offset = max_y ? std::clamp(view_.pan_y / max_y, 0.f, 1.f) : 0;
        const float within = s.y > kViewport.h && max_y > 0 ? p.offset : 1.f;
        svc::library().set_position(item_.id, p, (page_ + within) / doc_->page_count());
    }
    void bookmark() {
        save(true);
        svc::Bookmark b; b.label = "Page " + std::to_string(page_ + 1); b.created = time(nullptr);
        if (const auto* item = svc::library().find(item_.id)) b.at = item->position;
        svc::library().add_bookmark(item_.id, b); app().toast("Bookmark added");
    }
    void jump() {
        open_keyboard("Go to page (1-" + std::to_string(doc_->page_count()) + ")", std::to_string(page_ + 1), KeyboardKind::Text,
            [this](const std::string& text) {
                char* end = nullptr; const long page = strtol(text.c_str(), &end, 10);
                if (text.empty() || !end || *end || page < 1 || page > doc_->page_count()) app().toast("Enter a valid page number.");
                else go((int)page - 1);
            });
    }
    void contents() {
        if (outline_.empty()) { app().toast("This PDF has no table of contents."); return; }
        std::vector<MenuItem> items;
        for (const auto& o : outline_) items.push_back({std::string(std::max(0, o.level - 1) * 2, ' ') + o.title,
            [o] { return std::to_string(o.page + 1); }, [this, o] { history_.push_back({page_, view_}); go(o.page); }, nullptr});
        open_menu("PDF contents", std::move(items));
    }
    void navigation() {
        std::vector<MenuItem> items = {
            {"Page", [this] { return std::to_string(page_ + 1) + " / " + std::to_string(doc_->page_count()); }, nullptr, [this](int d) { go(page_ + d); }},
            {"Jump to page", nullptr, [this] { jump(); }, nullptr},
            {"Contents", nullptr, [this] { contents(); }, nullptr},
            {"Bookmark this page", nullptr, [this] { bookmark(); }, nullptr},
            {"Bookmarks", nullptr, [this] {
                std::vector<MenuItem> marks;
                if (const auto* item = svc::library().find(item_.id)) for (const auto& b : item->bookmarks)
                    marks.push_back({b.label, nullptr, [this, b] {
                        history_.push_back({page_, view_}); go(b.at.section);
                        resume_ = b.at; restore_ = true;
                        // Reuse the resume loader even for metadata already cached.
                        metadata_.erase(page_);
                    }, nullptr});
                if (marks.empty()) app().toast("No PDF bookmarks yet.");
                else open_menu("PDF bookmarks", std::move(marks));
            }, nullptr}
        };
        open_menu("PDF navigation", std::move(items));
    }
    void settings_menu() {
        std::vector<MenuItem> items = {
            {"Fit Page", nullptr, [this] { fit(0); }, nullptr},
            {"Fit Width", nullptr, [this] { fit(1); }, nullptr},
            {"Actual Size / 100%", nullptr, [this] { fit(2); }, nullptr},
            {"Zoom", [this] { return std::to_string((int)std::lround(view_.scale() / (96.f / 72) * 100)) + "%"; },
                nullptr, [this](int d) { zoom(d * 10); }},
            {"Zoom In", nullptr, [this] { zoom(10); }, nullptr},
            {"Zoom Out", nullptr, [this] { zoom(-10); }, nullptr},
            {"Page navigation", nullptr, [this] { navigation(); }, nullptr},
            {"Jump to page", nullptr, [this] { jump(); }, nullptr},
            {"Contents", nullptr, [this] { contents(); }, nullptr},
            {"Rotation", [this] { return std::to_string(view_.rotation * 90) + " degrees"; }, nullptr, [this](int d) { rotate(d); }},
            {"Continuous scrolling", [] { return std::string(svc::settings().pdf_continuous ? "On" : "Off"); }, nullptr,
                [this](int) { svc::settings().pdf_continuous = !svc::settings().pdf_continuous; view_.clamp(svc::settings().pdf_continuous, page_ == doc_->page_count() - 1); changed(); preferences(); }},
            {"Background", [] { const char* names[] = {"Dark", "Grey", "Sepia", "Black"}; return std::string(names[svc::settings().pdf_background]); }, nullptr,
                [this](int d) { auto& s = svc::settings(); s.pdf_background = (s.pdf_background + 4 + d) % 4; preferences(); }},
            {"Right stick", [this] { return std::string(pan_mode_ ? "Pan" : "Cursor"); }, nullptr,
                [this](int) { pan_mode_ = !pan_mode_; pointer_.hide(); }},
            {"Cursor speed", [] { return std::to_string(svc::settings().cursor_speed) + "%"; }, nullptr,
                [](int d) { auto& s = svc::settings(); s.cursor_speed = std::clamp(s.cursor_speed + d * 25, 25, 250); svc::settings_save(); }},
            {"Reset View", nullptr, [this] {
                view_.rotation = 0; svc::settings().pdf_continuous = false; svc::settings().pdf_background = 0;
                pan_mode_ = false; fit(0);
            }, nullptr}
        };
        open_menu("PDF reading tools", std::move(items), "Left / Right changes values. R3 switches cursor and pan modes.");
    }
    struct History { int page; PdfView view; };
    svc::Item item_;
    std::shared_ptr<doc::PagedDocument> doc_;
    PdfView view_;
    Pointer pointer_;
    std::map<int, Metadata> metadata_;
    std::map<TileKey, Tile> tiles_;
    std::vector<doc::OutlineEntry> outline_;
    std::vector<History> history_;
    int page_ = 0, generation_ = 0;
    size_t bytes_ = 0;
    uint64_t clock_ = 0, last_tick_ = 0, bar_until_ = 0, saved_at_ = 0;
    bool busy_ = false, pan_mode_ = false, restore_ = false, destination_ = false;
    float dest_x_ = 0, dest_y_ = 0;
    svc::Position resume_;
    std::string error_;
};
}  // namespace
void open_pdf_reader(const svc::Item& item, std::shared_ptr<doc::PagedDocument> document) {
    app().push(std::make_unique<PdfReader>(item, std::move(document)));
}
}  // namespace ui
