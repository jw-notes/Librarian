// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Drawing helpers shared by the screens: the header with its tabs, the footer
// of button prompts, cards and covers, spinners, messages, and the list/grid
// focus logic.
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "gfx/font.h"
#include "ui/screen.h"
#include "ui/theme.h"

namespace ui {

// Layout grid for 1920x1080, inside the TV-safe area.
constexpr int kMarginX = 96;
constexpr int kHeaderH = 132;
constexpr int kContentTop = 156;
constexpr int kFooterTop = 996;
constexpr int kContentW = 1920 - 2 * kMarginX;

constexpr int kTitlePx = 46;
constexpr int kHeadPx = 34;
constexpr int kBodyPx = 28;
constexpr int kSmallPx = 23;

void draw_background(gfx::Canvas& c);
void draw_header(gfx::Canvas& c, Tab active);
void draw_footer(gfx::Canvas& c, const std::vector<Hint>& hints);
// Draws a DualShock button glyph centred vertically on `cy`; returns its width.
int draw_button_glyph(gfx::Canvas& c, uint32_t button, int x, int cy, int size);
void draw_spinner(gfx::Canvas& c, int cx, int cy, int radius);
void draw_loading(gfx::Canvas& c, const gfx::Rect& area, const std::string& label);
void draw_message(gfx::Canvas& c, const gfx::Rect& area, const std::string& title, const std::string& body,
                  bool error = false);
void draw_focus(gfx::Canvas& c, const gfx::Rect& r, int radius);
void draw_progress(gfx::Canvas& c, const gfx::Rect& r, float value);
// A cover image, or a generated cover (colour from the title) until it loads.
void draw_cover(gfx::Canvas& c, const gfx::Rect& r, const std::string& image, const std::string& title,
                const std::string& author = "");
// A rounded panel in the surface colour.
void draw_panel(gfx::Canvas& c, const gfx::Rect& r, bool focused = false);
void draw_scrollbar(gfx::Canvas& c, const gfx::Rect& track, int first, int visible, int total);
std::string clock_text();

// Focus for a vertical list: index, and the first visible row.
struct ListNav {
    int index = 0;
    int top = 0;
    // Handles Up/Down (and Left/Right as page jumps when `paging`); true if moved.
    bool update(const Input& in, int count, int visible, bool paging = true);
    void clamp(int count, int visible);
};

// Focus for a grid of `cols` columns.
struct GridNav {
    int index = 0;
    int top_row = 0;
    bool update(const Input& in, int count, int cols, int visible_rows);
    void clamp(int count, int cols, int visible_rows);
};

}  // namespace ui
