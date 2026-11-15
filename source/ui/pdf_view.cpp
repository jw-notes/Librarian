// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/pdf_view.h"
#include <algorithm>
#include <cmath>
namespace ui {
float PdfView::scale() const {
    const float w = std::max(1.f, rotation % 2 ? page.height : page.width);
    const float h = std::max(1.f, rotation % 2 ? page.width : page.height);
    if (fit == 0) return std::min(viewport_w / w, viewport_h / h);
    if (fit == 1) return viewport_w / w;
    return std::clamp(zoom, 25, 400) / 100.f * (96.f / 72);
}
PdfPoint PdfView::size() const {
    return {std::ceil((rotation % 2 ? page.height : page.width) * scale()),
            std::ceil((rotation % 2 ? page.width : page.height) * scale())};
}
PdfPoint PdfView::project(float x, float y) const {
    PdfPoint p;
    switch (rotation % 4) {
        case 1: p = {page.height - y, x}; break;
        case 2: p = {page.width - x, page.height - y}; break;
        case 3: p = {y, page.width - x}; break;
        default: p = {x, y}; break;
    }
    return {p.x * scale(), p.y * scale()};
}
PdfPoint PdfView::unproject(float x, float y) const {
    x /= scale(); y /= scale();
    switch (rotation % 4) {
        case 1: return {y, page.height - x};
        case 2: return {page.width - x, page.height - y};
        case 3: return {page.width - y, x};
        default: return {x, y};
    }
}
gfx::Rect PdfView::link_rect(const doc::PageLink& l, int ox, int oy) const {
    const auto a = project(l.x, l.y), b = project(l.x + l.w, l.y + l.h);
    const int x = (int)std::floor(std::min(a.x, b.x)), y = (int)std::floor(std::min(a.y, b.y));
    return {ox + x, oy + y, (int)std::ceil(std::max(a.x, b.x)) - x, (int)std::ceil(std::max(a.y, b.y)) - y};
}
void PdfView::clamp(bool continuous, bool last) {
    const auto s = size();
    pan_x = std::clamp(pan_x, 0.f, std::max(0.f, s.x - viewport_w));
    const float max_y = continuous && !last ? s.y + 23 : std::max(0.f, s.y - viewport_h);
    pan_y = std::clamp(pan_y, 0.f, max_y);
}
void PdfView::zoom_to(int percent) {
    const float old = scale();
    const auto before = size();
    const float x = (pan_x + std::min((float)viewport_w, before.x) / 2.f) / old;
    const float y = (pan_y + std::min((float)viewport_h, before.y) / 2.f) / old;
    fit = 2; zoom = std::clamp(percent, 25, 400);
    pan_x = x * scale() - viewport_w / 2.f;
    pan_y = y * scale() - viewport_h / 2.f;
    clamp();
}
}  // namespace ui
