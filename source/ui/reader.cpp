// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/reader.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>

#include "audio/player.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/layout.h"
#include "gfx/font.h"
#include "gfx/image_cache.h"
#include "services/listening.h"
#include "services/settings.h"
#include "ui/app.h"
#include "ui/i18n.h"
#include "ui/overlays.h"
#include "ui/pointer.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

bool g_read_aloud_next = false;

constexpr int kTop = 96;            // content starts below the title line
constexpr int kBottomBar = 64;
constexpr int kContentH = kHeight - kTop - kBottomBar;

int column_width(int margins) {
    static const int widths[] = {1560, 1300, 1060};
    return widths[std::max(0, std::min(2, margins))];
}

class ReaderScreen : public Screen {
public:
    ReaderScreen(std::unique_ptr<ReaderSource> source, const svc::Position* start) : src_(std::move(source)) {
        paged_ = src_->is_article() ? svc::settings().articles_paged : svc::settings().books_paged;
        svc::Position from;
        if (start) {
            from = *start;
        } else if (const svc::Item* item = svc::library().find(src_->item_id())) {
            from = item->position;
        }
        from.section = std::max(0, std::min(from.section, src_->sections() - 1));
        Pending p;
        p.kind = Pending::At;
        p.pos = from;
        read_aloud_on_open_ = g_read_aloud_next;
        g_read_aloud_next = false;
        load(from.section, p);
        src_->open_url = [this](const std::string& url) { open_article(url); };
    }

    ~ReaderScreen() override {
        if (narrating_) audio::player().close();
        save_position(true);
        src_->open_url = nullptr;
    }

    bool full_screen() const override { return true; }
    bool animating() const override {
        return loading_ || laying_out_ || view_y_ != target_y_ || pointer_.shown() || narrating_ || narr_loading_;
    }

    void resumed() override { pointer_.hide(); }

    // ---- input -------------------------------------------------------------------

    void update(const Input& in) override {
        if (desired_style() != style_ || paged_ != wants_paged()) restyle();

        if (in.press(kCircle)) {
            back();
            return;
        }
        if (!layout_) {
            if (in.press(kCross) && !error_.empty()) retry();
            if (in.press(kTriangle)) open_reader_menu();
            return;
        }
        if (narrating_) follow_narration();
        if (in.press(kTriangle)) { pointer_.hide(); open_reader_menu(); return; }
        if (in.press(kOptions)) { pointer_.hide(); open_display_menu(); return; }
        if (in.press(kSquare)) add_bookmark();
        if (in.press(kL1)) {
            if (src_->is_article()) back();
            else if (section_ > 0) {
                go_section(section_ - 1, Pending::Start);
                narrate_section(section_);
            }
        }
        if (in.press(kR1) && !src_->is_article() && section_ + 1 < src_->sections()) {
            go_section(section_ + 1, Pending::Start);
            narrate_section(section_);
        }
        if (pointer_.update(in.rx, in.ry, now_ms(), svc::settings().cursor_speed)) request_redraw();
        if (in.rx || in.ry) focus_link_ = -1;
        if (in.repeat(kL2)) { pointer_.hide(); move_focus(-1); }
        if (in.repeat(kR2)) { pointer_.hide(); move_focus(1); }
        const bool scrolling = in.down(kUp | kDown | kLeft | kRight) || in.ly || view_y_ != target_y_;
        // Reading aloud: Cross pauses and resumes (links stay on the pointer and L2 / R2).
        if (narrating_ && in.press(kCross) && !pointer_.shown() && focus_link_ < 0) {
            audio::player().toggle();
        } else if (in.press(kCross) && !scrolling && !loading_ && !laying_out_) {
            if (pointer_.shown()) {
                const int link = hovered_link();
                if (link >= 0) follow(link);
            } else if (focus_link_ >= 0) follow(focus_link_);
            else move_focus(1);
        }

        const int line = std::max(24, style_.font_px * style_.line_spacing / 100);
        if (paged_) {
            if (in.repeat(kRight) || in.repeat(kDown)) next_page();
            if (in.repeat(kLeft) || in.repeat(kUp)) previous_page();
        } else {
            const int max_y = std::max(0, layout_->height - kContentH);
            if (in.repeat(kDown)) {
                if (target_y_ >= max_y && in.press(kDown)) next_section_or_stop();
                else target_y_ += line * 2;
            }
            if (in.repeat(kUp)) {
                if (target_y_ <= 0 && in.press(kUp) && section_ > 0) go_section(section_ - 1, Pending::End);
                else target_y_ -= line * 2;
            }
            if (in.repeat(kRight)) target_y_ += kContentH - line * 2;
            if (in.repeat(kLeft)) target_y_ -= kContentH - line * 2;

            target_y_ = std::max(0, std::min(target_y_, max_y));
            // Ease towards the target; snap when close.
            const int diff = target_y_ - view_y_;
            view_y_ += std::abs(diff) <= 2 ? diff : diff * 2 / 5;
        }
        if (focus_link_ >= 0 && !link_visible(focus_link_)) focus_link_ = -1;
        save_position(false);
    }

    // ---- drawing -----------------------------------------------------------------

    void draw(gfx::Canvas& c) override {
        const ReaderColors rc = reader_colors(svc::settings().reader_theme);
        c.clear(rc.page);
        const int cw = style_.width, cx = (kWidth - cw) / 2;

        // Title line.
        const std::string title = src_->title();
        const std::string section = src_->sections() > 1 ? current_heading() : std::string();
        std::string head = section.empty() || section == title ? title : title + "  \xC2\xB7  " + section;
        gfx::draw_text(c, Face::Sans, 24, kMarginX, 36, gfx::ellipsize(Face::Sans, 24, head, kWidth - 2 * kMarginX - 200),
                       rc.muted);
        if (layout_) {
            const std::string pct = std::to_string((int)lround(progress() * 100)) + "%";
            gfx::draw_text(c, Face::SansSemiBold, 24, kWidth - kMarginX - gfx::text_width(Face::SansSemiBold, 24, pct), 36,
                           pct, rc.muted);
        }
        if (loading_ || laying_out_) draw_spinner(c, kWidth - kMarginX - 110, 52, 12);

        if (!layout_) {
            const Rect area{0, kTop, kWidth, kContentH};
            if (!error_.empty()) {
                draw_message(c, area, "Could not open this page", error_ + "\n", true);
                const int y = area.y + area.h / 2 + 140;
                const int x = kWidth / 2 - 210;
                int w = draw_button_glyph(c, kCross, x, y, 40);
                gfx::draw_text(c, Face::Sans, 26, x + w + 12, y - 17, "Try again", rc.muted);
                w = draw_button_glyph(c, kCircle, x + 250, y, 40);
                gfx::draw_text(c, Face::Sans, 26, x + 250 + w + 12, y - 17, "Back", rc.muted);
            } else {
                draw_loading(c, area, "Loading\xE2\x80\xA6");
            }
            return;
        }

        const int top = view_top();
        const int bottom = view_bottom();
        c.push_clip({0, kTop, kWidth, bottom - top});
        const auto& frags = layout_->frags;
        // Hit-test against the same fragments and viewport that are drawn.
        const int highlight = pointer_.shown() ? hovered_link() : focus_link_;
        // The passage being read aloud.
        if (active_mark_ >= 0) {
            // One band per line, from its first marked word to its last.
            std::vector<Rect> bands;
            for (size_t i = layout_->first_visible(top); i < frags.size() && frags[i].y < bottom; ++i) {
                const doc::Frag& f = frags[i];
                if (f.mark != active_mark_ || f.kind != doc::Frag::Text) continue;
                Rect* band = nullptr;
                for (Rect& b : bands)
                    if (b.y == f.y) band = &b;
                if (!band) {
                    bands.push_back({f.x, f.y, f.w, f.h});
                    continue;
                }
                const int right = std::max(band->right(), f.x + f.w);
                band->x = std::min(band->x, f.x);
                band->w = right - band->x;
            }
            for (const Rect& b : bands)
                c.fill_rounded({cx + b.x - 6, kTop + b.y - top + 3, b.w + 12, b.h - 6}, 8, gfx::with_alpha(rc.link, 64));
        }
        if (highlight >= 0) {
            for (size_t i = layout_->first_visible(top); i < frags.size() && frags[i].y < bottom; ++i) {
                const doc::Frag& f = frags[i];
                if (f.link != highlight) continue;
                c.fill_rounded({cx + f.x - 6, kTop + f.y - top + 2, f.w + 12, f.h - 4}, 8, rc.link_focus_bg);
            }
        }
        for (size_t i = layout_->first_visible(top); i < frags.size() && frags[i].y < bottom; ++i) {
            const doc::Frag& f = frags[i];
            if (f.y + f.h <= top) continue;
            const int x = cx + f.x, y = kTop + f.y - top;
            switch (f.kind) {
                case doc::Frag::Text: {
                    const gfx::Color color = role_color(rc, f.role);
                    const int baseline = kTop + f.baseline - top;
                    gfx::draw_text_baseline(c, f.face, f.px, x, baseline, f.text, color);
                    if ((f.decoration & doc::kUnderline) || (f.link >= 0 && f.link == highlight))
                        c.fill({x, baseline + std::max(2, f.px / 10), f.w, std::max(1, f.px / 16)}, color);
                    if (f.decoration & doc::kStrike) c.fill({x, baseline - f.px / 3, f.w, std::max(1, f.px / 16)}, color);
                    break;
                }
                case doc::Frag::Box:
                    c.fill({x, y, f.w, f.h}, role_color(rc, f.role));
                    break;
                case doc::Frag::Image:
                    draw_image(c, f, x, y, rc);
                    if (f.link >= 0 && f.link == highlight) {
                        c.fill({x, y, f.w, f.h}, gfx::with_alpha(rc.link, 40));
                        c.stroke_rounded({x, y, f.w, f.h}, 4, 3, rc.link);
                    }
                    break;
            }
        }
        c.pop_clip();
        // Scrolling: fade the lines cut by the bottom edge.
        if (!paged_) {
            const int fade = 48, y0 = kTop + kContentH - fade;
            for (int i = 0; i < fade; ++i) c.fill({0, y0 + i, kWidth, 1}, gfx::with_alpha(rc.page, i * 255 / fade));
        }

        // Bottom line: position.
        const int by = kHeight - kBottomBar + 14;
        if (!narrating_ && !narr_loading_ && src_->has_narration()) {
            // A talking book: how to have it read aloud.
            const int gx = kMarginX;
            int w = draw_button_glyph(c, kTriangle, gx, by + 14, 30);
            gfx::draw_text(c, Face::Sans, 22, gx + w + 10, by, T("Read aloud"), rc.muted);
        }
        if (narrating_ || narr_loading_) {
            const audio::State st = audio::player().state();
            std::string label = narr_loading_ ? "Preparing the narrationâ¦"
                                : st == audio::State::Paused ? "Narration paused" : "Reading aloud";
            label = T(label);
            const int gx = kMarginX;
            int w = draw_button_glyph(c, kCross, gx, by + 14, 30);
            gfx::draw_text(c, Face::Sans, 22, gx + w + 10, by, label, rc.link);
        }
        if (paged_ && !pages_.empty()) {
            std::string label = "Page " + std::to_string(page_ + 1) + " of " + std::to_string(pages_.size());
            if (src_->sections() > 1)
                label += "   \xC2\xB7   Chapter " + std::to_string(section_ + 1) + " of " + std::to_string(src_->sections());
            const int w = gfx::text_width(Face::Sans, 22, label);
            gfx::draw_text(c, Face::Sans, 22, (kWidth - w) / 2, by, label, rc.muted);
        } else {
            const int bar_w = cw;
            c.fill_rounded({cx, by + 14, bar_w, 4}, 2, gfx::with_alpha(rc.muted, 70));
            const int max_y = std::max(1, layout_->height - kContentH);
            c.fill_rounded({cx, by + 14, std::max(4, (int)((int64_t)bar_w * std::min(view_y_, max_y) / max_y)), 4}, 2,
                           rc.link);
        }
        pointer_.draw(c, hovered_link() >= 0);
        if (pointer_.shown()) gfx::draw_text(c, Face::Sans, 20, kMarginX, kHeight - 38,
            "Right stick: point    Cross: open link    Options: display", rc.muted);
    }

private:
    int hovered_link() const {
        if (!layout_ || !pointer_.shown() || loading_ || laying_out_) return -1;
        return text_link_at(*layout_, pointer_.x(), pointer_.y(), (kWidth - style_.width) / 2,
                            kTop, view_top(), std::min(kContentH, view_bottom() - view_top()));
    }
    struct Pending {
        enum Kind { None, Start, End, At, Anchor, Block } kind = None;
        svc::Position pos;
        std::string anchor;
        int block = 0;
    };

    // The contents entry for the reading position: the last one that starts at
    // or before it (several entries may share one file).
    std::string current_heading() const {
        const auto toc = src_->toc();
        std::string title = src_->section_title(section_);
        const int block = layout_ ? layout_->block_at(view_top()) : 0;
        int best = -1;
        for (const TocItem& t : toc) {
            if (t.section != section_) continue;
            int at = 0;
            if (!t.anchor.empty() && doc_) {
                auto found = doc_->anchors.find(t.anchor);
                if (found == doc_->anchors.end()) continue;
                at = found->second;
            }
            if (at <= block && at >= best) {
                best = at;
                title = t.title;
            }
        }
        return title;
    }

    // ---- loading and layout ------------------------------------------------------

    doc::ReaderStyle desired_style() const {
        const svc::Settings& s = svc::settings();
        doc::ReaderStyle st;
        st.font_px = s.font_px;
        st.line_spacing = s.line_spacing;
        st.serif = s.serif;
        st.justify = s.justify;
        st.width = column_width(s.margins);
        st.viewport_h = kContentH;
        return st;
    }

    bool wants_paged() const { return src_->is_article() ? svc::settings().articles_paged : svc::settings().books_paged; }

    void load(int section, const Pending& pending) {
        save_position(true);
        section_ = section;
        loading_ = true;
        error_.clear();
        focus_link_ = -1;
        const int generation = ++generation_;
        const doc::ReaderStyle style = desired_style();
        std::shared_ptr<ReaderSource> src = src_;
        struct Result {
            LoadResult load;
            std::shared_ptr<doc::Document> doc;
            std::shared_ptr<doc::Layout> layout;
            bool ok = false;
        };
        lib::async<Result>(
            token_,
            [src, section, style]() {
                Result r;
                const uint64_t t0 = now_ms();
                r.ok = src->load(section, r.load);
                if (r.ok) {
                    r.doc = std::make_shared<doc::Document>(std::move(r.load.doc));
                    const uint64_t t1 = now_ms();
                    r.layout = std::make_shared<doc::Layout>(doc::layout_document(*r.doc, style));
                    LOG("reader: section %d loaded in %llu ms, laid out in %llu ms (%zu blocks, %zu frags, %d px)", section,
                        (unsigned long long)(t1 - t0), (unsigned long long)(now_ms() - t1), r.doc->blocks.size(),
                        r.layout->frags.size(), r.layout->height);
                }
                return r;
            },
            [this, generation, style, pending](Result& r) {
                if (generation != generation_) return;
                loading_ = false;
                if (!r.ok) {
                    error_ = r.load.error.empty() ? "The document could not be read." : r.load.error;
                    LOG("reader: section %d failed: %s", section_, error_.c_str());
                    doc_.reset();
                    layout_.reset();
                    return;
                }
                // The document moved into r.doc; the source wants its title.
                r.load.doc.title = r.doc->title;
                src_->loaded(section_, r.load);
                doc_ = r.doc;
                layout_ = r.layout;
                style_ = style;
                paged_ = wants_paged();
                pages_ = layout_->page_starts(kContentH);
                apply(pending);
                save_position(true);
                if (first_load_) {
                    first_load_ = false;
                    if (src_->has_narration() && (read_aloud_on_open_ || src_->narrate_on_open())) start_narration();
                }
            },
            2);
    }

    void retry() {
        Pending p;
        p.kind = Pending::Start;
        load(section_, p);
    }

    // Lays the current document out again (new text size, font, margins).
    void restyle() {
        if (!doc_ || laying_out_) {
            paged_ = wants_paged();
            return;
        }
        Pending p;
        p.kind = Pending::At;
        p.pos = position();
        laying_out_ = true;
        const doc::ReaderStyle style = desired_style();
        const std::shared_ptr<doc::Document> d = doc_;
        const int generation = generation_;
        lib::async<std::shared_ptr<doc::Layout>>(
            token_, [d, style]() { return std::make_shared<doc::Layout>(doc::layout_document(*d, style)); },
            [this, style, p, generation](std::shared_ptr<doc::Layout>& layout) {
                laying_out_ = false;
                if (generation != generation_) return;
                layout_ = layout;
                style_ = style;
                paged_ = wants_paged();
                pages_ = layout_->page_starts(kContentH);
                apply(p);
            },
            2);
    }

    void apply(const Pending& p) {
        int y = 0;
        switch (p.kind) {
            case Pending::None:
            case Pending::Start: y = 0; break;
            case Pending::End: y = layout_->height; break;
            case Pending::At: y = y_for(p.pos); break;
            case Pending::Block: y = block_y(p.block); break;
            case Pending::Anchor: {
                auto found = doc_->anchors.find(p.anchor);
                y = found == doc_->anchors.end() ? 0 : block_y(found->second);
                break;
            }
        }
        jump_to(y);
    }

    // ---- positions ---------------------------------------------------------------

    int view_top() const { return paged_ ? (pages_.empty() ? 0 : pages_[page_]) : view_y_; }
    int view_bottom() const {
        if (!paged_) return view_y_ + kContentH;
        return page_ + 1 < (int)pages_.size() ? pages_[page_ + 1] : pages_[page_] + kContentH;
    }

    int block_y(int block) const {
        if (!layout_ || layout_->block_top.empty()) return 0;
        block = std::max(0, std::min(block, (int)layout_->block_top.size() - 1));
        return layout_->block_top[block];
    }

    int y_for(const svc::Position& p) const {
        if (!layout_ || layout_->block_top.empty()) return 0;
        const int top = block_y(p.block);
        const int next = p.block + 1 < (int)layout_->block_top.size() ? layout_->block_top[p.block + 1] : layout_->height;
        return top + (int)((next - top) * std::max(0.f, std::min(1.f, p.offset)));
    }

    svc::Position position() const {
        svc::Position p;
        p.section = section_;
        if (!layout_ || layout_->block_top.empty()) return p;
        const int y = view_top();
        p.block = layout_->block_at(y);
        const int top = layout_->block_top[p.block];
        const int next = p.block + 1 < (int)layout_->block_top.size() ? layout_->block_top[p.block + 1] : layout_->height;
        p.offset = next > top ? (float)(y - top) / (next - top) : 0.f;
        return p;
    }

    double progress() const {
        if (!layout_) return 0;
        double within;
        if (paged_) within = pages_.size() <= 1 ? 1.0 : (double)page_ / (pages_.size() - 1);
        else within = layout_->height <= kContentH ? 1.0 : (double)view_y_ / (layout_->height - kContentH);
        within = std::max(0.0, std::min(1.0, within));
        double total = 0, before = 0;
        for (int i = 0; i < src_->sections(); ++i) {
            const double w = src_->section_weight(i);
            if (i < section_) before += w;
            total += w;
        }
        return total > 0 ? (before + within * src_->section_weight(section_)) / total : within;
    }

    void jump_to(int y) {
        if (!layout_) return;
        if (paged_) {
            auto it = std::upper_bound(pages_.begin(), pages_.end(), y);
            page_ = it == pages_.begin() ? 0 : (int)(it - pages_.begin()) - 1;
        } else {
            view_y_ = target_y_ = std::max(0, std::min(y, std::max(0, layout_->height - kContentH)));
        }
        focus_link_ = -1;
    }

    void save_position(bool now) {
        if (!layout_ || src_->item_id().empty()) return;
        const uint64_t t = now_ms();
        if (!now && t - last_save_ < 1500) return;
        last_save_ = t;
        svc::library().set_position(src_->item_id(), position(), (float)progress());
    }

    // ---- navigation --------------------------------------------------------------

    void next_page() {
        if (page_ + 1 < (int)pages_.size()) {
            ++page_;
            focus_link_ = -1;
        } else {
            next_section_or_stop();
        }
    }

    void previous_page() {
        if (page_ > 0) {
            --page_;
            focus_link_ = -1;
        } else if (section_ > 0 && !src_->is_article()) {
            go_section(section_ - 1, Pending::End);
        }
    }

    void next_section_or_stop() {
        if (!src_->is_article() && section_ + 1 < src_->sections()) {
            go_section(section_ + 1, Pending::Start);
        } else if (!src_->is_article()) {
            app().toast("End of the book");
            if (svc::Item* item = svc::library().find(src_->item_id())) {
                item->finished = true;
                svc::library().mark_dirty();
            }
        }
    }

    void go_section(int section, Pending::Kind kind) {
        Pending p;
        p.kind = kind;
        load(section, p);
    }

    void back() {
        if (!history_.empty()) {
            const History h = history_.back();
            history_.pop_back();
            Pending p;
            p.kind = Pending::At;
            p.pos = h.pos;
            if (src_->is_article() && h.url != src_->current_url()) {
                src_->navigate(h.url);
                load(0, p);
            } else if (h.pos.section != section_) {
                load(h.pos.section, p);
            } else {
                apply(p);
            }
            return;
        }
        save_position(true);
        app().pop();
    }

    void remember() { history_.push_back({src_->current_url(), position()}); }

    void open_article(const std::string& url) {
        if (layout_) remember();
        save_position(true);
        src_->navigate(url);
        Pending p;
        p.kind = Pending::Start;
        load(0, p);
    }

    // ---- links -------------------------------------------------------------------

    std::vector<int> visible_links() const {
        std::vector<int> links;
        if (!layout_) return links;
        const int top = view_top(), bottom = view_bottom();
        const auto& frags = layout_->frags;
        for (size_t i = layout_->first_visible(top); i < frags.size() && frags[i].y < bottom; ++i) {
            const doc::Frag& f = frags[i];
            if (f.link < 0 || f.y < top || f.y + f.h > bottom + 2) continue;
            if (std::find(links.begin(), links.end(), f.link) == links.end()) links.push_back(f.link);
        }
        return links;
    }

    bool link_visible(int link) const {
        const auto links = visible_links();
        return std::find(links.begin(), links.end(), link) != links.end();
    }

    void move_focus(int delta) {
        const auto links = visible_links();
        if (links.empty()) {
            focus_link_ = -1;
            return;
        }
        auto it = std::find(links.begin(), links.end(), focus_link_);
        if (it == links.end()) {
            focus_link_ = delta > 0 ? links.front() : links.back();
            return;
        }
        const int i = (int)(it - links.begin()) + delta;
        focus_link_ = links[(i + (int)links.size()) % (int)links.size()];
    }

    void follow(int link) {
        if (!doc_ || link < 0 || link >= (int)doc_->links.size()) return;
        const std::string href = doc_->links[link];
        const LinkTarget t = src_->resolve(href, section_);
        switch (t.kind) {
            case LinkTarget::Section: {
                remember();
                Pending p;
                p.kind = t.anchor.empty() ? Pending::Start : Pending::Anchor;
                p.anchor = t.anchor;
                if (t.section == section_) apply(p);
                else load(t.section, p);
                break;
            }
            case LinkTarget::Article:
                open_article(t.url);
                break;
            case LinkTarget::External:
                app().toast("Web links cannot be opened here: " + t.url.substr(0, 60));
                break;
            case LinkTarget::None:
                app().toast("This link cannot be opened.");
                break;
        }
    }

    // ---- images ------------------------------------------------------------------

    void draw_image(gfx::Canvas& c, const doc::Frag& f, int x, int y, const ReaderColors& rc) {
        if (const gfx::Image* img = gfx::image_get(f.text, f.w, f.h, gfx::Fit::Contain)) {
            // Transparent images (diagrams) get a light backing so dark lines
            // stay visible on dark themes.
            if (!img->opaque) c.fill_rounded({x + (f.w - img->lw()) / 2 - 8, y + (f.h - img->lh()) / 2 - 8, img->lw() + 16, img->lh() + 16}, 8, gfx::rgb(0xF4F4F2));
            c.draw_image(*img, x + (f.w - img->lw()) / 2, y + (f.h - img->lh()) / 2);
            return;
        }
        c.fill_rounded({x, y, f.w, f.h}, 8, rc.image_placeholder);
        if (gfx::image_state(f.text) == gfx::ImageState::Failed) {
            const char* label = "Image unavailable";
            const int w = gfx::text_width(Face::Sans, 22, label);
            gfx::draw_text(c, Face::Sans, 22, x + (f.w - w) / 2, y + f.h / 2 - 14, label, rc.muted);
        }
    }

    // ---- menus -------------------------------------------------------------------

    void add_bookmark() {
        if (!layout_) return;
        svc::Bookmark b;
        b.at = position();
        b.created = time(nullptr);
        // Label: the first words on screen.
        std::string words;
        const int top = view_top();
        for (size_t i = layout_->first_visible(top); i < layout_->frags.size() && words.size() < 70; ++i) {
            const doc::Frag& f = layout_->frags[i];
            if (f.y < top || f.kind != doc::Frag::Text || f.role == doc::Role::Marker) continue;
            words += (words.empty() ? "" : " ") + f.text;
        }
        b.label = gfx::ellipsize(Face::Sans, 24, lib::collapse_space(words), 600);
        svc::library().add_bookmark(src_->item_id(), b);
        app().toast("Bookmark added");
    }

    // ---- read-along -------------------------------------------------------------

    // The block a narrated passage starts in (its mark, or its anchor).
    int narrated_block(const std::string& id) const {
        if (!doc_) return -1;
        auto m = doc_->mark_of.find(id);
        if (m != doc_->mark_of.end() && doc_->mark_block[m->second] >= 0) return doc_->mark_block[m->second];
        auto a = doc_->anchors.find(id);
        return a == doc_->anchors.end() ? -1 : a->second;
    }

    void start_narration() {
        if (narr_) {
            begin_narration();
            return;
        }
        if (narr_loading_) return;
        narr_loading_ = true;
        std::shared_ptr<ReaderSource> src = src_;
        lib::async<std::pair<std::shared_ptr<doc::Timeline>, std::string>>(
            token_,
            [src] {
                auto t = std::make_shared<doc::Timeline>();
                std::string error;
                if (!src->narration(*t, error)) t.reset();
                return std::make_pair(t, error);
            },
            [this](std::pair<std::shared_ptr<doc::Timeline>, std::string>& r) {
                narr_loading_ = false;
                if (!r.first) {
                    app().toast(r.second.empty() ? "This book's narration could not be read." : r.second);
                    return;
                }
                narr_ = r.first;
                begin_narration();
            });
    }

    // Plays from the first passage on the page being read.
    void begin_narration() {
        if (!narr_ || !layout_) return;
        const int block = layout_->block_at(view_top());
        int i = narr_->first_from(section_, block, [this](const std::string& id) { return narrated_block(id); });
        if (i < 0) i = 0;
        play_narration_from(i);
    }

    void play_narration_from(int i) {
        // The narration takes over the player from an audiobook (its place is saved).
        if (svc::listening().active()) svc::listening().stop();
        audio::player().open(narr_->tracks, nullptr, narr_->points[i].at, true, narration_speed_);
        narrating_ = true;
        narr_point_ = -1;
        active_mark_ = -1;
        LOG("reader: reading aloud from passage %d (%.1f s)", i, narr_->points[i].at);
    }

    // The reader moved to another chapter by itself: the narration follows.
    void narrate_section(int section) {
        if (!narrating_ || !narr_) return;
        for (size_t i = 0; i < narr_->points.size(); ++i)
            if (narr_->points[i].section == section) {
                audio::player().seek(narr_->points[i].at);
                narr_point_ = -1;
                active_mark_ = -1;
                return;
            }
        // A part with no narration (front matter, notes): reading aloud stops
        // rather than pulling the reader back.
        stop_narration();
        app().toast("This part is not narrated: reading aloud stopped.");
    }

    void stop_narration() {
        if (!narrating_) return;
        narration_speed_ = audio::player().speed();
        audio::player().close();
        narrating_ = false;
        active_mark_ = -1;
        narr_point_ = -1;
    }

    // Each frame while reading aloud: the passage playing now, highlighted
    // and kept on screen (another chapter is opened when the narration gets there).
    void follow_narration() {
        const audio::State st = audio::player().state();
        if (st == audio::State::Ended || st == audio::State::Idle) {
            stop_narration();
            return;
        }
        if (st == audio::State::Error) {
            app().toast(audio::player().error());
            stop_narration();
            return;
        }
        const int i = narr_->find(audio::player().position());
        if (i < 0) return;
        const doc::SyncPoint& p = narr_->points[i];
        if (p.section >= 0 && p.section != section_) {
            if (!loading_ && wanted_section_ != p.section) {
                wanted_section_ = p.section;
                Pending pd;
                pd.kind = p.id.empty() ? Pending::Start : Pending::Anchor;
                pd.anchor = p.id;
                load(p.section, pd);
            }
            return;
        }
        wanted_section_ = -1;
        if (loading_ || laying_out_ || !layout_ || i == narr_point_) return;
        narr_point_ = i;
        auto m = doc_->mark_of.find(p.id);
        active_mark_ = m == doc_->mark_of.end() ? -1 : m->second;
        // Its place on the page: the marked text, or else its block.
        int y0 = -1, y1 = -1;
        if (active_mark_ >= 0)
            for (const doc::Frag& f : layout_->frags)
                if (f.mark == active_mark_) {
                    if (y0 < 0 || f.y < y0) y0 = f.y;
                    y1 = std::max(y1, f.y + f.h);
                }
        if (y0 < 0) {
            const int b = narrated_block(p.id);
            if (b < 0) return;
            y0 = block_y(b);
            y1 = y0 + 1;
        }
        if (y0 < view_top() || y1 > view_bottom()) jump_to(paged_ ? y0 : y0 - kContentH / 4);
    }

    void open_reader_menu() {
        std::vector<MenuItem> items;
        if (doc_) {
            items.push_back({"Contents", nullptr, [this] { open_contents(); }, nullptr});
        }
        items.push_back({"Bookmarks", [this] {
                             const svc::Item* it = svc::library().find(src_->item_id());
                             return std::to_string(it ? it->bookmarks.size() : 0);
                         },
                         [this] { open_bookmarks(); }, nullptr});
        if (layout_) items.push_back({"Add bookmark here", nullptr, [this] { add_bookmark(); }, nullptr});
        const std::string id = src_->item_id();
        items.push_back({"Favourite", [id] {
                             const svc::Item* it = svc::library().find(id);
                             return std::string(it && it->favourite ? "Yes" : "No");
                         },
                         [id] {
                             const svc::Item* it = svc::library().find(id);
                             svc::library().set_favourite(id, !(it && it->favourite));
                         },
                         nullptr, true});
        src_->add_menu_items(items);
        if (src_->has_narration()) {
            if (narrating_) {
                items.push_back({"Stop reading aloud", nullptr, [this] { stop_narration(); }, nullptr});
                items.push_back({"Narration speed",
                                 [] {
                                     char b[16];
                                     snprintf(b, sizeof b, "%.2gx", audio::player().speed());
                                     return std::string(b);
                                 },
                                 nullptr,
                                 [](int d) {
                                     const double s = std::max(0.5, std::min(3.0, audio::player().speed() + 0.25 * d));
                                     audio::player().set_speed(s);
                                 },
                                 true});
            } else {
                items.insert(items.begin(), {"Read aloud", nullptr, [this] { start_narration(); }, nullptr});
            }
        }
        items.push_back({"Display settings", nullptr, [this] { open_display_menu(); }, nullptr});
        items.push_back({"Close", nullptr, [this] {
                             save_position(true);
                             app().pop();
                         },
                         nullptr});
        open_menu(src_->title(), std::move(items));
    }

    void open_contents() {
        std::vector<TocItem> toc = src_->toc();
        if (toc.empty() && doc_) {
            for (const auto& e : doc_->toc)
                if (e.level >= 2 && e.level <= 3) toc.push_back({e.title, section_, "", e.block, e.level});
            if (toc.empty())
                for (const auto& e : doc_->toc) toc.push_back({e.title, section_, "", e.block, e.level});
        }
        if (toc.empty()) {
            app().toast("This document has no table of contents.");
            return;
        }
        std::vector<MenuItem> items;
        for (const TocItem& t : toc) {
            const std::string label = std::string(std::max(0, t.level - (src_->toc().empty() ? 2 : 1)) * 3, ' ') + t.title;
            items.push_back({label, nullptr, [this, t] {
                                 remember();
                                 Pending p;
                                 if (!t.anchor.empty()) {
                                     p.kind = Pending::Anchor;
                                     p.anchor = t.anchor;
                                 } else if (t.block >= 0) {
                                     p.kind = Pending::Block;
                                     p.block = t.block;
                                 } else {
                                     p.kind = Pending::Start;
                                 }
                                 if (t.section == section_ && layout_) apply(p);
                                 else load(t.section, p);
                             },
                             nullptr});
        }
        open_menu("Contents", std::move(items));
    }

    void open_bookmarks() {
        const svc::Item* item = svc::library().find(src_->item_id());
        if (!item || item->bookmarks.empty()) {
            app().toast("No bookmarks yet. Press Square while reading to add one.");
            return;
        }
        std::vector<MenuItem> items;
        for (size_t i = 0; i < item->bookmarks.size(); ++i) {
            const svc::Bookmark b = item->bookmarks[i];
            std::string where = src_->sections() > 1 ? "Ch. " + std::to_string(b.at.section + 1) + "  " : "";
            items.push_back({where + (b.label.empty() ? "Bookmark" : b.label), nullptr,
                             [this, b, i] {
                                 std::vector<MenuItem> actions;
                                 actions.push_back({"Go to bookmark", nullptr, [this, b] {
                                                        remember();
                                                        Pending p;
                                                        p.kind = Pending::At;
                                                        p.pos = b.at;
                                                        if (b.at.section == section_ && layout_) apply(p);
                                                        else load(b.at.section, p);
                                                    },
                                                    nullptr});
                                 MenuItem del{"Delete bookmark", nullptr, [this, i] {
                                                  svc::library().remove_bookmark(src_->item_id(), i);
                                                  app().toast("Bookmark deleted");
                                              },
                                              nullptr};
                                 del.destructive = true;
                                 actions.push_back(del);
                                 open_menu("Bookmark", std::move(actions), b.label);
                             },
                             nullptr});
        }
        open_menu("Bookmarks", std::move(items));
    }

    void open_display_menu() {
        std::vector<MenuItem> items;
        auto save = [] { svc::settings_save(); };
        items.push_back({"Text size", [] { return std::to_string(svc::settings().font_px) + " px"; }, nullptr,
                         [save](int d) {
                             svc::settings().font_px = std::max(22, std::min(64, svc::settings().font_px + d * 2));
                             save();
                         }});
        items.push_back({"Typeface", [] { return std::string(svc::settings().serif ? "Serif" : "Sans serif"); }, nullptr,
                         [save](int) {
                             svc::settings().serif = !svc::settings().serif;
                             save();
                         }});
        items.push_back({"Line spacing", [] { return std::to_string(svc::settings().line_spacing) + "%"; }, nullptr,
                         [save](int d) {
                             svc::settings().line_spacing = std::max(110, std::min(200, svc::settings().line_spacing + d * 10));
                             save();
                         }});
        items.push_back({"Margins", [] {
                             static const char* names[] = {"Narrow", "Normal", "Wide"};
                             return std::string(names[svc::settings().margins]);
                         },
                         nullptr,
                         [save](int d) {
                             svc::settings().margins = (svc::settings().margins + 3 + d) % 3;
                             save();
                         }});
        items.push_back({"Justify text", [] { return std::string(svc::settings().justify ? "On" : "Off"); }, nullptr,
                         [save](int) {
                             svc::settings().justify = !svc::settings().justify;
                             save();
                         }});
        items.push_back({"Theme", [] {
                             static const char* names[] = {"Dark", "Light", "Sepia", "Black"};
                             return std::string(names[(int)svc::settings().reader_theme]);
                         },
                         nullptr,
                         [save](int d) {
                             svc::settings().reader_theme = (svc::ReaderTheme)(((int)svc::settings().reader_theme + 4 + d) % 4);
                             save();
                         }});
        const bool article = src_->is_article();
        items.push_back({article ? "Articles: layout" : "Books: layout",
                         [article] {
                             const bool paged = article ? svc::settings().articles_paged : svc::settings().books_paged;
                             return std::string(paged ? "Pages" : "Scrolling");
                         },
                         nullptr,
                         [article, save](int) {
                             bool& v = article ? svc::settings().articles_paged : svc::settings().books_paged;
                             v = !v;
                             save();
                         }});
        items.push_back({"Cursor speed", [] { return std::to_string(svc::settings().cursor_speed) + "%"; }, nullptr,
                         [save](int d) { svc::settings().cursor_speed = std::clamp(svc::settings().cursor_speed + d * 25, 25, 250); save(); }});
        open_menu("Display", std::move(items), "Changes apply as you make them.");
    }

    struct History {
        std::string url;
        svc::Position pos;
    };

    // Shared with load jobs, which may still be running when the reader closes.
    std::shared_ptr<ReaderSource> src_;
    int section_ = 0;
    std::shared_ptr<doc::Document> doc_;
    std::shared_ptr<doc::Layout> layout_;
    doc::ReaderStyle style_;
    bool paged_ = false, loading_ = false, laying_out_ = false;
    // Read-along.
    std::shared_ptr<doc::Timeline> narr_;
    bool narrating_ = false, narr_loading_ = false;
    int narr_point_ = -1, active_mark_ = -1, wanted_section_ = -1;
    double narration_speed_ = 1.0;
    bool first_load_ = true, read_aloud_on_open_ = false;
    std::string error_;
    int view_y_ = 0, target_y_ = 0;
    std::vector<int> pages_;
    int page_ = 0;
    Pointer pointer_;
    int focus_link_ = -1;
    int generation_ = 0;
    uint64_t last_save_ = 0;
    std::vector<History> history_;
};

}  // namespace

void request_read_aloud() { g_read_aloud_next = true; }

void open_reader(std::unique_ptr<ReaderSource> source, const svc::Position* start) {
    app().push(std::make_unique<ReaderScreen>(std::move(source), start));
}

}  // namespace ui
