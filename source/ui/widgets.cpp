// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/widgets.h"
#include "ui/i18n.h"

#include <math.h>
#include <time.h>

#include <algorithm>

#include "core/worker.h"
#include "gfx/image_cache.h"
#include "platform/platform.h"
#include "ui/app.h"

namespace ui {

using gfx::Canvas;
using gfx::Color;
using gfx::Face;
using gfx::Rect;
using namespace platform;

void draw_background(Canvas& c) {
    const Theme& t = theme();
    c.fill_vgradient({0, 0, c.width(), c.height()}, t.bg_top, t.bg);
}

std::string clock_text() {
    const time_t now = time(nullptr) + utc_offset_seconds();
    tm t{};
    gmtime_r(&now, &t);
    char text[16];
    if (clock_24h()) snprintf(text, sizeof text, "%02d:%02d", t.tm_hour, t.tm_min);
    else snprintf(text, sizeof text, "%d:%02d %s", (t.tm_hour + 11) % 12 + 1, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    return text;
}

// The wordmark's mark: three book spines.
static void draw_logo(Canvas& c, int x, int y, int size) {
    const Theme& t = theme();
    const int w = size / 5;
    c.fill_rounded({x, y + size / 6, w, size - size / 6}, 3, t.accent);
    c.fill_rounded({x + w + 4, y, w, size}, 3, gfx::mix(t.accent, t.text, 120));
    // A leaning third book.
    for (int i = 0; i < 3; ++i) {
        const float x0 = x + 2 * (w + 4) + i * 1.0f, y0 = y + size;
        c.line(x0 + w / 2.f, y0 - 2, x0 + w / 2.f + size * 0.28f, y0 - size * 0.92f, (float)w, t.muted);
    }
}

void draw_header(Canvas& c, Tab active) {
    const Theme& t = theme();
    draw_logo(c, kMarginX, 46, 44);
    gfx::draw_text(c, Face::SansBold, 40, kMarginX + 72, 40, "Librarian", t.text);

    std::vector<Tab> tabs;
    for (int i = 0; i < (int)Tab::Count; ++i)
        if (tab_visible((Tab)i)) tabs.push_back((Tab)i);
    // Compact pills, centred on the same line as the title (y 69).
    const int px = 25, pad = 18, gap = 6;
    int total = 0;
    for (Tab tab : tabs) total += gfx::text_width(Face::SansSemiBold, px, tab_name(tab)) + 2 * pad + gap;
    total -= gap;
    int x = (c.width() - total) / 2;
    const int h = 46, y = 69 - h / 2;
    for (Tab tab : tabs) {
        const int w = gfx::text_width(Face::SansSemiBold, px, tab_name(tab)) + 2 * pad;
        const bool on = active == tab;
        if (on) c.fill_rounded({x, y, w, h}, h / 2, t.accent);
        gfx::draw_text(c, Face::SansSemiBold, px, x + pad, y + (h - gfx::font_metrics(Face::SansSemiBold, px).line_height) / 2,
                       tab_name(tab), on ? t.accent_text : t.muted);
        x += w + gap;
    }
    // L1 / R1 either side of the tabs.
    const int first_x = (c.width() - total) / 2;
    draw_button_glyph(c, kL1, first_x - 54, y + h / 2, 30);
    draw_button_glyph(c, kR1, x - gap + 14, y + h / 2, 30);

    const std::string clock = clock_text();
    const int cw = gfx::text_width(Face::SansSemiBold, 28, clock);
    gfx::draw_text(c, Face::SansSemiBold, 28, c.width() - kMarginX - cw, 69 - gfx::font_metrics(Face::SansSemiBold, 28).line_height / 2,
                   clock, t.text);
    if (lib::jobs_in_flight() > 0) draw_spinner(c, c.width() - kMarginX - cw - 36, 69, 11);
}

int draw_button_glyph(Canvas& c, uint32_t button, int x, int cy, int size) {
    const Theme& t = theme();
    const int r = size / 2;
    const int cx = x + r;
    const Color ring = t.text;
    switch (button) {
        case kCross: {
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const float d = r * 0.42f;
            c.line(cx - d, cy - d, cx + d, cy + d, size / 10.f, gfx::rgb(0x8FB4FF));
            c.line(cx - d, cy + d, cx + d, cy - d, size / 10.f, gfx::rgb(0x8FB4FF));
            return size;
        }
        case kCircle:
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            c.stroke_circle(cx, cy, (int)(r * 0.55f), std::max(2, size / 11), gfx::rgb(0xFF7B88));
            return size;
        case kSquare: {
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const int s = (int)(r * 0.95f);
            c.stroke_rounded({cx - s / 2, cy - s / 2, s, s}, 2, std::max(2, size / 11), gfx::rgb(0xF5A3D7));
            return size;
        }
        case kTriangle: {
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const float s = r * 0.55f, th = size / 11.f;
            const Color g = gfx::rgb(0x5FD3B8);
            c.line(cx, cy - s, cx - s * 0.95f, cy + s * 0.65f, th, g);
            c.line(cx - s * 0.95f, cy + s * 0.65f, cx + s * 0.95f, cy + s * 0.65f, th, g);
            c.line(cx + s * 0.95f, cy + s * 0.65f, cx, cy - s, th, g);
            return size;
        }
        default: {
            // Shoulder buttons, Options and the touch pad as labelled keycaps;
            // directions as a small D-pad.
            const char* label = button == kL1 ? "L1" : button == kR1 ? "R1" : button == kL2 ? "L2" : button == kR2 ? "R2"
                              : button == kOptions ? "OPTIONS" : button == kTouchPad ? "TOUCH PAD" : nullptr;
            if (label) {
                const int px = size * 52 / 100;
                const int w = gfx::text_width(Face::SansBold, px, label) + size * 2 / 3;
                c.fill_rounded({x, cy - r, w, size}, size / 4, gfx::rgb(0x2B3446));
                gfx::draw_text(c, Face::SansBold, px, x + size / 3, cy - gfx::font_metrics(Face::SansBold, px).line_height / 2,
                               label, gfx::rgb(0xDDE3EE));
                return w;
            }
            c.fill_circle(cx, cy, r, gfx::rgb(0x2B3446));
            const int arm = size / 6, len = (int)(r * 0.75f);
            c.fill({cx - arm / 2, cy - len, arm, len * 2}, ring);
            c.fill({cx - len, cy - arm / 2, len * 2, arm}, ring);
            const Color hi = theme().accent;
            if (button & kUp) c.fill({cx - arm / 2, cy - len, arm, len / 2}, hi);
            if (button & kDown) c.fill({cx - arm / 2, cy + len / 2, arm, len / 2}, hi);
            if (button & kLeft) c.fill({cx - len, cy - arm / 2, len / 2, arm}, hi);
            if (button & kRight) c.fill({cx + len / 2, cy - arm / 2, len / 2, arm}, hi);
            return size;
        }
    }
}

void draw_footer(Canvas& c, const std::vector<Hint>& hints) {
    const Theme& t = theme();
    const int px = 25, size = 38, cy = kFooterTop + 40;
    int x = c.width() - kMarginX;
    // Right-aligned, in the order given (the first hint is rightmost).
    for (const Hint& h : hints) {
        const std::string label = T(h.label);
        const int lw = gfx::text_width(Face::Sans, px, label);
        x -= lw;
        gfx::draw_text(c, Face::Sans, px, x, cy - gfx::font_metrics(Face::Sans, px).line_height / 2, label, t.muted);
        // Measure the glyph by drawing it off-screen first is wasteful; glyph widths are known.
        int gw = size;
        if (h.button == kL1 || h.button == kR1 || h.button == kL2 || h.button == kR2 || h.button == kOptions ||
            h.button == kTouchPad) {
            const char* label = h.button == kOptions ? "OPTIONS" : h.button == kTouchPad ? "TOUCH PAD" : "L1";
            gw = gfx::text_width(Face::SansBold, size * 52 / 100, label) + size * 2 / 3;
        }
        x -= gw + 12;
        draw_button_glyph(c, h.button, x, cy, size);
        x -= 44;
    }
}

void draw_spinner(Canvas& c, int cx, int cy, int radius) {
    const Theme& t = theme();
    const double phase = (now_ms() % 1200) / 1200.0 * 2 * M_PI;
    for (int i = 0; i < 10; ++i) {
        const double a = phase + i * 2 * M_PI / 10;
        const int alpha = 40 + i * 215 / 9;
        c.fill_circle(cx + (int)(cos(a) * radius), cy + (int)(sin(a) * radius), std::max(2, radius / 5),
                      gfx::with_alpha(t.accent, alpha));
    }
}

void draw_loading(Canvas& c, const Rect& area, const std::string& label_en) {
    const Theme& t = theme();
    const std::string label = T(label_en);
    const int cy = area.y + area.h / 2 - 30;
    draw_spinner(c, area.x + area.w / 2, cy, 28);
    const int w = gfx::text_width(Face::Sans, kBodyPx, label);
    gfx::draw_text(c, Face::Sans, kBodyPx, area.x + (area.w - w) / 2, cy + 52, label, t.muted);
}

void draw_message(Canvas& c, const Rect& area, const std::string& title_en, const std::string& body_en, bool error) {
    const Theme& t = theme();
    const std::string title = T(title_en), body = T(body_en);
    const int width = std::min(area.w, 1100);
    const auto lines = gfx::wrap_text(Face::Sans, kBodyPx, body, width, 8);
    const int line_h = gfx::font_metrics(Face::Sans, kBodyPx).line_height + 6;
    const int height = 70 + (int)lines.size() * line_h;
    int y = area.y + (area.h - height) / 2;
    const int tw = gfx::text_width(Face::SansBold, kHeadPx, title);
    gfx::draw_text(c, Face::SansBold, kHeadPx, area.x + (area.w - tw) / 2, y, title, error ? t.error : t.text);
    y += 70;
    for (const auto& line : lines) {
        const int lw = gfx::text_width(Face::Sans, kBodyPx, line);
        gfx::draw_text(c, Face::Sans, kBodyPx, area.x + (area.w - lw) / 2, y, line, t.muted);
        y += line_h;
    }
}

void draw_focus(Canvas& c, const Rect& r, int radius) {
    const Theme& t = theme();
    c.stroke_rounded({r.x - 6, r.y - 6, r.w + 12, r.h + 12}, radius + 6, 4, t.focus);
}

void draw_progress(Canvas& c, const Rect& r, float value) {
    const Theme& t = theme();
    c.fill_rounded(r, r.h / 2, gfx::with_alpha(t.text, 50));
    const int w = (int)(r.w * std::max(0.f, std::min(1.f, value)));
    if (w > 0) c.fill_rounded({r.x, r.y, std::max(w, r.h), r.h}, r.h / 2, t.accent);
}

void draw_panel(Canvas& c, const Rect& r, bool focused) {
    const Theme& t = theme();
    c.fill_rounded(r, 14, focused ? t.surface_hi : t.surface);
}

void draw_cover(Canvas& c, const Rect& r, const std::string& image, const std::string& title, const std::string& author) {
    if (const gfx::Image* img = image.empty() ? nullptr : gfx::image_get(image, r.w, r.h, gfx::Fit::Cover)) {
        c.draw_image_rounded(*img, r.x + (r.w - img->lw()) / 2, r.y + (r.h - img->lh()) / 2, 10);
        return;
    }
    // A generated cover: a colour from the title, with the title on it.
    uint32_t hash = 2166136261u;
    for (char ch : title) hash = (hash ^ (uint8_t)ch) * 16777619u;
    static const Color palette[] = {gfx::rgb(0x3D5A80), gfx::rgb(0x6D597A), gfx::rgb(0x355C4B), gfx::rgb(0x8A5A44),
                                    gfx::rgb(0x2F4858), gfx::rgb(0x5B4B8A), gfx::rgb(0x7A4E5F), gfx::rgb(0x46607A)};
    const Color base = palette[hash % 8];
    c.fill_rounded(r, 10, base);
    c.fill({r.x + r.w / 14, r.y, 3, r.h}, gfx::with_alpha(gfx::rgb(0x000000), 60));
    const int px = std::max(16, r.w / 9);
    int y = r.y + r.h / 7;
    const int pad = r.w / 8;
    y += gfx::draw_wrapped(c, Face::SerifBold, px, r.x + pad, y, r.w - 2 * pad, title, gfx::rgb(0xF4F1EA), 4);
    if (!author.empty())
        gfx::draw_wrapped(c, Face::Sans, px * 3 / 4, r.x + pad, y + px / 2, r.w - 2 * pad, author, gfx::rgb(0xD9D4C7), 2);
}

void draw_scrollbar(Canvas& c, const Rect& track, int first, int visible, int total) {
    if (total <= visible || total <= 0) return;
    const Theme& t = theme();
    c.fill_rounded(track, track.w / 2, gfx::with_alpha(t.text, 30));
    const int h = std::max(track.w * 3, track.h * visible / total);
    const int y = track.y + (int)((int64_t)(track.h - h) * first / std::max(1, total - visible));
    c.fill_rounded({track.x, y, track.w, h}, track.w / 2, gfx::with_alpha(t.text, 140));
}

bool ListNav::update(const Input& in, int count, int visible, bool paging) {
    if (count <= 0) return false;
    const int before = index;
    if (in.repeat(kUp)) index = index > 0 ? index - 1 : (in.press(kUp) ? count - 1 : 0);
    if (in.repeat(kDown)) index = index < count - 1 ? index + 1 : (in.press(kDown) ? 0 : count - 1);
    if (paging && in.repeat(kLeft)) index = std::max(0, index - visible);
    if (paging && in.repeat(kRight)) index = std::min(count - 1, index + visible);
    clamp(count, visible);
    return index != before;
}

void ListNav::clamp(int count, int visible) {
    index = std::max(0, std::min(index, count - 1));
    if (index < top) top = index;
    if (index >= top + visible) top = index - visible + 1;
    top = std::max(0, std::min(top, std::max(0, count - visible)));
}

bool GridNav::update(const Input& in, int count, int cols, int visible_rows) {
    if (count <= 0) return false;
    const int before = index;
    if (in.repeat(kLeft) && index % cols > 0) --index;
    if (in.repeat(kRight) && index % cols < cols - 1 && index + 1 < count) ++index;
    if (in.repeat(kUp) && index >= cols) index -= cols;
    if (in.repeat(kDown)) {
        if (index + cols < count) index += cols;
        else if (index / cols < (count - 1) / cols) index = count - 1;  // onto the shorter last row
    }
    clamp(count, cols, visible_rows);
    return index != before;
}

void GridNav::clamp(int count, int cols, int visible_rows) {
    index = std::max(0, std::min(index, count - 1));
    const int row = index / cols;
    if (row < top_row) top_row = row;
    if (row >= top_row + visible_rows) top_row = row - visible_rows + 1;
    top_row = std::max(0, top_row);
}

}  // namespace ui
