// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Text rendering with stb_truetype. Glyph bitmaps, advances and kerning are
// cached; the caches are shared by the UI thread and layout workers.
#include <string>
#include <string_view>
#include <vector>

#include "gfx/canvas.h"

namespace gfx {

enum class Face {
    Sans,
    SansSemiBold,
    SansBold,
    SansItalic,
    Serif,
    SerifBold,
    SerifItalic,
    SerifBoldItalic,
    Mono,
    Count
};

struct FontMetrics {
    int ascent = 0;       // pixels above the baseline
    int descent = 0;      // pixels below the baseline (positive)
    int line_height = 0;  // ascent + descent + line gap
};

bool fonts_init(const std::string& directory);
FontMetrics font_metrics(Face face, int px);

int text_width(Face face, int px, std::string_view text);
// Draws `text` with its top edge at `y`. Returns the advance width.
int draw_text(Canvas& canvas, Face face, int px, int x, int y, std::string_view text, Color color);
// Draws at a baseline (used by document layout, where runs share baselines).
int draw_text_baseline(Canvas& canvas, Face face, int px, int x, int baseline, std::string_view text, Color color);
// Shortens `text` with "…" so it fits in `max_width`.
std::string ellipsize(Face face, int px, std::string_view text, int max_width);
// Word-wraps `text` into at most `max_lines` lines (the last one ellipsized).
std::vector<std::string> wrap_text(Face face, int px, std::string_view text, int width, int max_lines = 1000);
// Draws a wrapped paragraph; returns the height used.
int draw_wrapped(Canvas& canvas, Face face, int px, int x, int y, int width, std::string_view text, Color color,
                 int max_lines = 1000, int line_height = 0);

}  // namespace gfx
