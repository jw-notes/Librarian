// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Software drawing into a 32-bit frame. Colours are 0xAARRGGBB; an alpha of
// 0xFF is opaque. Everything clips to the current clip rectangle.
//
// Resolution: every screen is laid out on a logical 1920 x 1080 canvas; the
// frame itself has the TV's resolution (1280 x 720, 1920 x 1080, 3840 x 2160).
// The canvas converts each shape to real pixels, text is rasterised at its
// real size (font.cpp) and pictures are decoded at it (Image::density), so
// nothing is drawn small and enlarged: 4K is sharp, 720p draws fewer pixels.
#include <stdint.h>

#include <vector>

namespace gfx {

using Color = uint32_t;
constexpr Color rgb(uint32_t v) { return 0xFF000000u | v; }
constexpr Color with_alpha(Color c, int alpha) { return ((uint32_t)alpha << 24) | (c & 0xFFFFFF); }
Color mix(Color a, Color b, int t);  // t: 0 -> a, 255 -> b

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    Rect intersect(const Rect& o) const;
};

struct Image;

// Real pixels per logical pixel: 1 at 1080p, 2/3 at 720p, 2 at 2160p. Set
// once, before drawing starts (text measuring and image loading use it too).
void set_render_scale(float scale);
float render_scale();

class Canvas {
public:
    // A frame of `pw` x `ph` real pixels showing a `lw` x `lh` logical canvas.
    Canvas(uint32_t* pixels, int pw, int ph, int lw, int lh);
    // Logical size (what layouts use).
    int width() const { return lw_; }
    int height() const { return lh_; }
    // Real pixels.
    int pixel_width() const { return pw_; }
    int pixel_height() const { return ph_; }
    float scale() const { return s_; }
    uint32_t* pixels() { return px_; }
    // Logical to real pixels.
    int px(int v) const;
    Rect px(const Rect& r) const;

    void push_clip(const Rect& r);
    void pop_clip();
    // The clip in real pixels.
    const Rect& clip() const { return clip_; }

    void clear(Color c);
    void fill(const Rect& r, Color c);
    void fill_rounded(const Rect& r, int radius, Color c);
    void stroke_rounded(const Rect& r, int radius, int thickness, Color c);
    void fill_vgradient(const Rect& r, Color top, Color bottom);
    void fill_circle(int cx, int cy, int radius, Color c);
    void stroke_circle(int cx, int cy, int radius, int thickness, Color c);
    void line(float x0, float y0, float x1, float y1, float thickness, Color c);
    // Draws `img` at its own (logical) size, with an optional extra opacity.
    // An image whose density is not the canvas's is resampled.
    void draw_image(const Image& img, int x, int y, int opacity = 255);
    // Like draw_image, with the corners rounded off.
    void draw_image_rounded(const Image& img, int x, int y, int radius);
    // In real pixels: one coverage value (0..255) of colour `c` at (x, y), and
    // a rectangle of them (a glyph bitmap). Used by text.
    inline void blend_pixel(int x, int y, Color c, int coverage);
    void blend_mask(int x, int y, const uint8_t* mask, int mw, int mh, int stride, Color c);

private:
    // The same operations in real pixels.
    void fill_raw(const Rect& r, Color c);
    void fill_rounded_raw(const Rect& r, int radius, Color c);
    void blit_raw(const Image& img, int x, int y, int opacity);
    void blit_rounded_raw(const Image& img, int x, int y, int radius);

    uint32_t* px_;
    int pw_, ph_, lw_, lh_;
    float s_;
    Rect clip_;
    std::vector<Rect> clips_;
};

inline void Canvas::blend_pixel(int x, int y, Color c, int coverage) {
    if (x < clip_.x || y < clip_.y || x >= clip_.right() || y >= clip_.bottom()) return;
    const int a = (int)((c >> 24) * (uint32_t)coverage) / 255;
    if (a <= 0) return;
    uint32_t& d = px_[y * pw_ + x];
    if (a >= 255) { d = c | 0xFF000000u; return; }
    const uint32_t keep = 255 - a;
    const uint32_t rb = (((c & 0xFF00FF) * a + (d & 0xFF00FF) * keep) >> 8) & 0xFF00FF;
    const uint32_t g = (((c & 0x00FF00) * a + (d & 0x00FF00) * keep) >> 8) & 0x00FF00;
    d = 0xFF000000u | rb | g;
}

}  // namespace gfx
