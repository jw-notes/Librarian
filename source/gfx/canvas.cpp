// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "gfx/canvas.h"

#include <math.h>

#include <algorithm>

#include "gfx/image.h"

namespace gfx {

Color mix(Color a, Color b, int t) {
    const int u = 255 - t;
    auto ch = [&](int shift) { return ((((a >> shift) & 255) * u + ((b >> shift) & 255) * t) / 255) << shift; };
    return (Color)(ch(24) | ch(16) | ch(8) | ch(0));
}

Rect Rect::intersect(const Rect& o) const {
    const int x1 = std::max(x, o.x), y1 = std::max(y, o.y);
    const int x2 = std::min(right(), o.right()), y2 = std::min(bottom(), o.bottom());
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

namespace {
float g_scale = 1.f;
}

void set_render_scale(float scale) { g_scale = scale > 0.1f ? scale : 1.f; }
float render_scale() { return g_scale; }

Canvas::Canvas(uint32_t* pixels, int pw, int ph, int lw, int lh)
    : px_(pixels), pw_(pw), ph_(ph), lw_(lw), lh_(lh), s_(lh > 0 ? (float)ph / lh : 1.f), clip_{0, 0, pw, ph} {}

int Canvas::px(int v) const { return s_ == 1.f ? v : (int)lroundf(v * s_); }

// Edges are converted (not the size), so neighbouring shapes still meet.
Rect Canvas::px(const Rect& r) const {
    if (s_ == 1.f) return r;
    const int x0 = px(r.x), y0 = px(r.y), x1 = px(r.right()), y1 = px(r.bottom());
    return {x0, y0, x1 - x0, y1 - y0};
}

void Canvas::push_clip(const Rect& r) {
    clips_.push_back(clip_);
    clip_ = clip_.intersect(px(r));
}

void Canvas::pop_clip() {
    if (clips_.empty()) return;
    clip_ = clips_.back();
    clips_.pop_back();
}

void Canvas::clear(Color c) { std::fill(px_, px_ + (size_t)pw_ * ph_, c | 0xFF000000u); }

void Canvas::fill(const Rect& r, Color c) { fill_raw(px(r), c); }

void Canvas::fill_raw(const Rect& r, Color c) {
    const Rect a = r.intersect(clip_);
    if (a.empty()) return;
    const int alpha = c >> 24;
    if (alpha == 0) return;
    if (alpha == 255) {
        for (int y = a.y; y < a.bottom(); ++y) std::fill(px_ + y * pw_ + a.x, px_ + y * pw_ + a.right(), c);
        return;
    }
    const uint32_t keep = 255 - alpha, rb = (c & 0xFF00FF) * alpha, g = (c & 0x00FF00) * alpha;
    for (int y = a.y; y < a.bottom(); ++y) {
        uint32_t* row = px_ + y * pw_;
        for (int x = a.x; x < a.right(); ++x) {
            const uint32_t d = row[x];
            row[x] = 0xFF000000u | ((((d & 0xFF00FF) * keep + rb) >> 8) & 0xFF00FF) |
                     ((((d & 0x00FF00) * keep + g) >> 8) & 0x00FF00);
        }
    }
}

// Coverage of a pixel row segment inside a rounded rectangle's corner.
static int corner_coverage(float dx, float dy, float radius) {
    const float d = sqrtf(dx * dx + dy * dy) - radius;
    if (d <= -0.5f) return 255;
    if (d >= 0.5f) return 0;
    return (int)((0.5f - d) * 255);
}

void Canvas::fill_rounded(const Rect& r, int radius, Color c) { fill_rounded_raw(px(r), px(radius), c); }

void Canvas::fill_rounded_raw(const Rect& r, int radius, Color c) {
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    if (radius <= 0) { fill_raw(r, c); return; }
    fill_raw({r.x, r.y + radius, r.w, r.h - 2 * radius}, c);
    for (int i = 0; i < radius; ++i) {
        // Rows inside the top and bottom bands: solid middle, anti-aliased corners.
        const float dy = radius - i - 0.5f;
        int inset = radius;
        for (int j = 0; j < radius; ++j) {
            if (corner_coverage(radius - j - 0.5f, dy, (float)radius) == 255) { inset = j; break; }
        }
        fill_raw({r.x + inset, r.y + i, r.w - 2 * inset, 1}, c);
        fill_raw({r.x + inset, r.bottom() - 1 - i, r.w - 2 * inset, 1}, c);
        for (int j = 0; j < inset; ++j) {
            const int cov = corner_coverage(radius - j - 0.5f, dy, (float)radius);
            if (!cov) continue;
            blend_pixel(r.x + j, r.y + i, c, cov);
            blend_pixel(r.right() - 1 - j, r.y + i, c, cov);
            blend_pixel(r.x + j, r.bottom() - 1 - i, c, cov);
            blend_pixel(r.right() - 1 - j, r.bottom() - 1 - i, c, cov);
        }
    }
}

void Canvas::stroke_rounded(const Rect& logical, int radius, int t, Color c) {
    const Rect r = px(logical);
    radius = px(radius);
    t = std::max(1, px(t));
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    const float ro = (float)radius, ri = (float)std::max(0, radius - t);
    fill_raw({r.x + radius, r.y, r.w - 2 * radius, t}, c);
    fill_raw({r.x + radius, r.bottom() - t, r.w - 2 * radius, t}, c);
    fill_raw({r.x, r.y + radius, t, r.h - 2 * radius}, c);
    fill_raw({r.right() - t, r.y + radius, t, r.h - 2 * radius}, c);
    for (int i = 0; i < radius; ++i) {
        for (int j = 0; j < radius; ++j) {
            const float dx = radius - j - 0.5f, dy = radius - i - 0.5f;
            const int outer = corner_coverage(dx, dy, ro);
            const int inner = radius > t ? corner_coverage(dx, dy, ri) : 0;
            const int cov = std::max(0, outer - inner);
            if (!cov) continue;
            blend_pixel(r.x + j, r.y + i, c, cov);
            blend_pixel(r.right() - 1 - j, r.y + i, c, cov);
            blend_pixel(r.x + j, r.bottom() - 1 - i, c, cov);
            blend_pixel(r.right() - 1 - j, r.bottom() - 1 - i, c, cov);
        }
    }
}

void Canvas::fill_vgradient(const Rect& logical, Color top, Color bottom) {
    const Rect r = px(logical);
    for (int i = 0; i < r.h; ++i) fill_raw({r.x, r.y + i, r.w, 1}, mix(top, bottom, r.h > 1 ? i * 255 / (r.h - 1) : 0));
}

void Canvas::fill_circle(int lcx, int lcy, int lradius, Color c) {
    const int cx = px(lcx), cy = px(lcy), radius = px(lradius);
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            const int cov = corner_coverage((float)x, (float)y, radius - 0.5f);
            if (cov) blend_pixel(cx + x, cy + y, c, cov);
        }
}

void Canvas::stroke_circle(int lcx, int lcy, int lradius, int lt, Color c) {
    const int cx = px(lcx), cy = px(lcy), radius = px(lradius), t = std::max(1, px(lt));
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            const int cov = std::max(0, corner_coverage((float)x, (float)y, radius - 0.5f) -
                                            corner_coverage((float)x, (float)y, radius - 0.5f - t));
            if (cov) blend_pixel(cx + x, cy + y, c, cov);
        }
}

void Canvas::line(float x0, float y0, float x1, float y1, float thickness, Color c) {
    x0 *= s_;
    y0 *= s_;
    x1 *= s_;
    y1 *= s_;
    thickness = std::max(1.f, thickness * s_);
    // Distance-to-segment coverage over the bounding box: fine for the short
    // strokes used by icons.
    const int bx0 = (int)floorf(std::min(x0, x1) - thickness), bx1 = (int)ceilf(std::max(x0, x1) + thickness);
    const int by0 = (int)floorf(std::min(y0, y1) - thickness), by1 = (int)ceilf(std::max(y0, y1) + thickness);
    const float vx = x1 - x0, vy = y1 - y0, len2 = std::max(1e-6f, vx * vx + vy * vy), half = thickness / 2;
    for (int y = by0; y <= by1; ++y)
        for (int x = bx0; x <= bx1; ++x) {
            const float px = x + 0.5f - x0, py = y + 0.5f - y0;
            const float t = std::max(0.f, std::min(1.f, (px * vx + py * vy) / len2));
            const float dx = px - t * vx, dy = py - t * vy;
            const float d = sqrtf(dx * dx + dy * dy) - half;
            if (d < 0.5f) blend_pixel(x, y, c, d <= -0.5f ? 255 : (int)((0.5f - d) * 255));
        }
}

namespace {

// `img` resampled to dw x dh (bilinear): an image whose density differs from
// the screen's (a PDF tile, a comic page made for another size).
Image resampled(const Image& img, int dw, int dh) {
    Image out;
    out.w = dw;
    out.h = dh;
    out.opaque = img.opaque;
    out.px.resize((size_t)dw * dh);
    const float fx = (float)img.w / dw, fy = (float)img.h / dh;
    for (int y = 0; y < dh; ++y) {
        const float sy = std::max(0.f, (y + 0.5f) * fy - 0.5f);
        const int y0 = std::min((int)sy, img.h - 1), y1 = std::min(y0 + 1, img.h - 1);
        const int wy = (int)((sy - y0) * 256);
        for (int x = 0; x < dw; ++x) {
            const float sx = std::max(0.f, (x + 0.5f) * fx - 0.5f);
            const int x0 = std::min((int)sx, img.w - 1), x1 = std::min(x0 + 1, img.w - 1);
            const int wx = (int)((sx - x0) * 256);
            const uint32_t a = img.px[(size_t)y0 * img.w + x0], b = img.px[(size_t)y0 * img.w + x1];
            const uint32_t c = img.px[(size_t)y1 * img.w + x0], d = img.px[(size_t)y1 * img.w + x1];
            uint32_t v = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                const int top = (int)((a >> sh) & 255) * (256 - wx) + (int)((b >> sh) & 255) * wx;
                const int bot = (int)((c >> sh) & 255) * (256 - wx) + (int)((d >> sh) & 255) * wx;
                v |= (uint32_t)(((top * (256 - wy) + bot * wy) >> 16) & 255) << sh;
            }
            out.px[(size_t)y * dw + x] = v;
        }
    }
    return out;
}

}  // namespace

void Canvas::draw_image(const Image& img, int x, int y, int opacity) {
    if (img.px.empty()) return;
    const float k = s_ / img.density;  // real pixels per image pixel
    if (fabsf(k - 1.f) < 0.01f) {
        blit_raw(img, px(x), px(y), opacity);
        return;
    }
    const int dw = std::max(1, (int)lroundf(img.w * k)), dh = std::max(1, (int)lroundf(img.h * k));
    // Only the visible part is resampled.
    const Rect dest{px(x), px(y), dw, dh};
    if (dest.intersect(clip_).empty()) return;
    blit_raw(resampled(img, dw, dh), dest.x, dest.y, opacity);
}

void Canvas::blit_raw(const Image& img, int x, int y, int opacity) {
    const Rect a = Rect{x, y, img.w, img.h}.intersect(clip_);
    if (a.empty() || img.px.empty()) return;
    for (int row = a.y; row < a.bottom(); ++row) {
        const uint32_t* src = img.px.data() + (size_t)(row - y) * img.w + (a.x - x);
        uint32_t* dst = px_ + row * pw_ + a.x;
        if (img.opaque && opacity >= 255) {
            std::copy(src, src + a.w, dst);
            continue;
        }
        for (int i = 0; i < a.w; ++i) {
            const uint32_t s = src[i];
            const int alpha = (int)(s >> 24) * opacity / 255;
            if (alpha <= 0) continue;
            if (alpha >= 255) { dst[i] = s; continue; }
            const uint32_t d = dst[i], keep = 255 - alpha;
            dst[i] = 0xFF000000u | ((((s & 0xFF00FF) * alpha + (d & 0xFF00FF) * keep) >> 8) & 0xFF00FF) |
                     ((((s & 0x00FF00) * alpha + (d & 0x00FF00) * keep) >> 8) & 0x00FF00);
        }
    }
}

void Canvas::draw_image_rounded(const Image& img, int x, int y, int radius) {
    if (img.px.empty()) return;
    const float k = s_ / img.density;
    if (fabsf(k - 1.f) < 0.01f) {
        blit_rounded_raw(img, px(x), px(y), px(radius));
        return;
    }
    const int dw = std::max(1, (int)lroundf(img.w * k)), dh = std::max(1, (int)lroundf(img.h * k));
    if (Rect{px(x), px(y), dw, dh}.intersect(clip_).empty()) return;
    blit_rounded_raw(resampled(img, dw, dh), px(x), px(y), px(radius));
}

void Canvas::blit_rounded_raw(const Image& img, int x, int y, int radius) {
    radius = std::min(radius, std::min(img.w, img.h) / 2);
    // The middle rows are a plain copy; corner rows are masked per pixel.
    const Rect saved = clip_;
    clip_ = clip_.intersect({x, y + radius, img.w, img.h - 2 * radius});
    blit_raw(img, x, y, 255);
    clip_ = saved;
    for (int i = 0; i < radius; ++i) {
        for (int band = 0; band < 2; ++band) {
            const int row = band == 0 ? i : img.h - 1 - i;
            const float dy = radius - i - 0.5f;
            for (int j = 0; j < img.w; ++j) {
                int cov = 255;
                if (j < radius) cov = corner_coverage(radius - j - 0.5f, dy, (float)radius);
                else if (j >= img.w - radius) cov = corner_coverage(j - (img.w - radius) + 0.5f, dy, (float)radius);
                if (!cov) continue;
                const uint32_t s = img.px[(size_t)row * img.w + j];
                blend_pixel(x + j, y + row, s | 0xFF000000u, cov * (int)(s >> 24) / 255);
            }
        }
    }
}

void Canvas::blend_mask(int x, int y, const uint8_t* mask, int mw, int mh, int stride, Color c) {
    const Rect a = Rect{x, y, mw, mh}.intersect(clip_);
    if (a.empty()) return;
    const int ca = c >> 24;
    for (int row = a.y; row < a.bottom(); ++row) {
        const uint8_t* m = mask + (row - y) * stride + (a.x - x);
        uint32_t* d = px_ + row * pw_ + a.x;
        for (int i = 0; i < a.w; ++i) {
            const int cov = m[i];
            if (!cov) continue;
            const int alpha = ca == 255 ? cov : cov * ca / 255;
            if (alpha >= 255) { d[i] = c | 0xFF000000u; continue; }
            const uint32_t keep = 255 - alpha, dst = d[i];
            d[i] = 0xFF000000u | ((((c & 0xFF00FF) * alpha + (dst & 0xFF00FF) * keep) >> 8) & 0xFF00FF) |
                   ((((c & 0x00FF00) * alpha + (dst & 0x00FF00) * keep) >> 8) & 0x00FF00);
        }
    }
}

}  // namespace gfx
