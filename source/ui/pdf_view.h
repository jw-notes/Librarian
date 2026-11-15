// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include "doc/paged.h"
namespace ui {
struct PdfPoint { float x = 0, y = 0; };
// The same transform is used for rendering, pan limits and link rectangles.
struct PdfView {
    doc::PageInfo page;
    int fit = 1, zoom = 100, rotation = 0;
    float pan_x = 0, pan_y = 0;
    int viewport_w = 1840, viewport_h = 1016;
    float scale() const;
    PdfPoint size() const;
    PdfPoint project(float x, float y) const;
    PdfPoint unproject(float x, float y) const;
    gfx::Rect link_rect(const doc::PageLink& link, int origin_x, int origin_y) const;
    void clamp(bool continuous = false, bool last_page = true);
    void zoom_to(int percent);
};
}  // namespace ui
