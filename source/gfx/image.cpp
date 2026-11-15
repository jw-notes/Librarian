// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "gfx/image.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_MAX_DIMENSIONS 16384
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
#include "webp/decode.h"

namespace gfx {

namespace {

bool is_webp(const std::string& d) {
    return d.size() > 12 && memcmp(d.data(), "RIFF", 4) == 0 && memcmp(d.data() + 8, "WEBP", 4) == 0;
}

// One output sample's sources along an axis: weights sum to 65536.
struct Tap { int first; std::vector<int> weights; };

std::vector<Tap> make_taps(int src, int dst) {
    std::vector<Tap> taps(dst);
    const double scale = (double)src / dst;
    for (int i = 0; i < dst; ++i) {
        Tap& t = taps[i];
        if (scale > 1.0) {
            // Shrinking: average the source span this pixel covers.
            const double a = i * scale, b = (i + 1) * scale;
            t.first = (int)floor(a);
            const int last = std::min(src - 1, (int)ceil(b) - 1);
            double total = 0;
            std::vector<double> w;
            for (int s = t.first; s <= last; ++s) {
                const double cover = std::min(b, s + 1.0) - std::max(a, (double)s);
                w.push_back(std::max(0.0, cover));
                total += w.back();
            }
            int sum = 0;
            for (double v : w) {
                t.weights.push_back((int)(v / total * 65536));
                sum += t.weights.back();
            }
            if (!t.weights.empty()) t.weights.back() += 65536 - sum;
        } else {
            // Enlarging: bilinear.
            const double centre = (i + 0.5) * scale - 0.5;
            int s0 = (int)floor(centre);
            double frac = centre - s0;
            if (s0 < 0) { s0 = 0; frac = 0; }
            if (s0 >= src - 1) { s0 = src - 1; frac = 0; }
            t.first = s0;
            const int w1 = (int)(frac * 65536);
            t.weights = {65536 - w1};
            if (w1) t.weights.push_back(w1);
        }
    }
    return taps;
}

// Scales a w x h source, read pixel by pixel through `px(x, y)` (0xAARRGGBB),
// to dw x dh. Separable: each source row is scaled horizontally once, and only
// the few rows the current output row needs are kept, so memory stays small
// however large the source is (a cover scan can be 4000 pixels tall).
template <typename Pixel>
void scale_rows(int sw, int sh, int dw, int dh, bool opaque, Pixel px, Image& out) {
    out.w = dw;
    out.h = dh;
    out.opaque = opaque;
    const auto xt = make_taps(sw, dw), yt = make_taps(sh, dh);
    // Premultiplied channels, horizontal pass into 16.16 fixed point / 256.
    auto horizontal = [&](int y, std::vector<uint32_t>& mid) {
        mid.resize((size_t)dw * 4);
        for (int x = 0; x < dw; ++x) {
            const Tap& t = xt[x];
            uint64_t a = 0, r = 0, g = 0, b = 0;
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const uint32_t p = px(t.first + (int)k, y);
                const uint64_t wgt = (uint64_t)t.weights[k], pa = p >> 24;
                a += pa * wgt;
                r += ((p >> 16) & 255) * pa * wgt / 255;
                g += ((p >> 8) & 255) * pa * wgt / 255;
                b += (p & 255) * pa * wgt / 255;
            }
            uint32_t* m = &mid[(size_t)x * 4];
            m[0] = (uint32_t)(a >> 8);
            m[1] = (uint32_t)(r >> 8);
            m[2] = (uint32_t)(g >> 8);
            m[3] = (uint32_t)(b >> 8);
        }
    };
    std::vector<std::vector<uint32_t>> rows;  // horizontally scaled source rows first_row ...
    int first_row = 0;
    out.px.resize((size_t)dw * dh);
    for (int y = 0; y < dh; ++y) {
        const Tap& t = yt[y];
        // Rows above this output row's span are no longer needed.
        while (!rows.empty() && first_row < t.first) {
            rows.erase(rows.begin());
            ++first_row;
        }
        if (rows.empty()) first_row = t.first;
        while (first_row + (int)rows.size() < t.first + (int)t.weights.size()) {
            rows.emplace_back();
            horizontal(first_row + (int)rows.size() - 1, rows.back());
        }
        for (int x = 0; x < dw; ++x) {
            uint64_t acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const uint32_t* m = &rows[t.first + k - first_row][(size_t)x * 4];
                for (int c = 0; c < 4; ++c) acc[c] += (uint64_t)m[c] * (uint64_t)t.weights[k];
            }
            // Back from (value * 256 * 65536) to 0..255.
            const uint32_t a = (uint32_t)std::min<uint64_t>(255, (acc[0] + (1u << 23)) >> 24);
            uint32_t rgb[3];
            for (int c = 0; c < 3; ++c) {
                const uint64_t v = (acc[c + 1] + (1u << 23)) >> 24;
                rgb[c] = a ? (uint32_t)std::min<uint64_t>(255, v * 255 / a) : 0;
            }
            out.px[(size_t)y * dw + x] = (a << 24) | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
        }
    }
}

// Big pictures are decoded one at a time: each needs its full size in memory
// while it is decoded, and the console has about 420 MB in all.
constexpr int64_t kBigPixels = 4 * 1000 * 1000;
// Larger than this, decoding alone could exhaust the console's memory: such a
// picture is not shown (a placeholder is) rather than risk the app.
constexpr int64_t kMaxPixels = 16 * 1000 * 1000;
pthread_mutex_t g_big_decode = PTHREAD_MUTEX_INITIALIZER;

}  // namespace

BigDecodeLock::BigDecodeLock() { pthread_mutex_lock(&g_big_decode); }
BigDecodeLock::~BigDecodeLock() { pthread_mutex_unlock(&g_big_decode); }

bool image_dimensions(const std::string& data, int& w, int& h) {
    if (is_webp(data)) return WebPGetInfo((const uint8_t*)data.data(), data.size(), &w, &h) != 0;
    int channels;
    return stbi_info_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &channels) != 0;
}

bool decode_image(const std::string& data, Image& out, int max_w, int max_h) {
    out = Image();
    int w = 0, h = 0;
    if (!image_dimensions(data, w, h) || w <= 0 || h <= 0) return false;
    const int64_t pixels_total = (int64_t)w * h;
    if (pixels_total > kMaxPixels) return false;
    const bool big = pixels_total > kBigPixels;
    if (big) pthread_mutex_lock(&g_big_decode);
    // The decoder's own buffer is read directly: no second full-size copy.
    bool ok = false;
    const uint32_t* argb = nullptr;  // WebP: 0xAARRGGBB words
    stbi_uc* rgba = nullptr;         // stb_image: R, G, B, A bytes
    int channels = 4;
    if (is_webp(data)) {
        // BGRA bytes are 0xAARRGGBB words on a little-endian machine.
        argb = (const uint32_t*)WebPDecodeBGRA((const uint8_t*)data.data(), data.size(), &w, &h);
    } else {
        rgba = stbi_load_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &channels, 4);
    }
    if (argb || rgba) {
        const size_t n = (size_t)w * h;
        auto px = [&](int x, int y) -> uint32_t {
            const size_t i = (size_t)y * w + x;
            if (argb) return argb[i];
            const stbi_uc* p = rgba + i * 4;
            return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        };
        bool opaque = true;
        // A JPEG (three channels) has no transparency to look for.
        if (argb || channels == 4)
            for (size_t i = 0; i < n && opaque; ++i) opaque = argb ? (argb[i] >> 24) == 255 : rgba[i * 4 + 3] == 255;
        if (w > max_w || h > max_h) {
            const double s = std::min((double)max_w / w, (double)max_h / h);
            scale_rows(w, h, std::max(1, (int)lround(w * s)), std::max(1, (int)lround(h * s)), opaque, px, out);
        } else {
            out.w = w;
            out.h = h;
            out.opaque = opaque;
            out.px.resize(n);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) out.px[(size_t)y * w + x] = px(x, y);
        }
        ok = true;
    }
    if (argb) WebPFree((void*)argb);
    if (rgba) stbi_image_free(rgba);
    if (big) pthread_mutex_unlock(&g_big_decode);
    return ok;
}

Image scale_image(const Image& src, int w, int h) {
    Image out;
    if (src.empty() || w <= 0 || h <= 0) return out;
    out.w = w;
    out.h = h;
    out.opaque = src.opaque;
    if (w == src.w && h == src.h) {
        out.px = src.px;
        return out;
    }
    const uint32_t* p = src.px.data();
    const int sw = src.w;
    scale_rows(src.w, src.h, w, h, src.opaque, [p, sw](int x, int y) { return p[(size_t)y * sw + x]; }, out);
    return out;
}

Image fit_image(const Image& src, int w, int h) {
    if (src.empty()) return Image();
    const double s = std::min((double)w / src.w, (double)h / src.h);
    return scale_image(src, std::max(1, (int)lround(src.w * s)), std::max(1, (int)lround(src.h * s)));
}

Image cover_image(const Image& src, int w, int h) {
    if (src.empty()) return Image();
    const double s = std::max((double)w / src.w, (double)h / src.h);
    // Crop the source to the target's aspect ratio first, then scale.
    const int cw = std::min(src.w, (int)lround(w / s)), ch = std::min(src.h, (int)lround(h / s));
    const int cx = (src.w - cw) / 2, cy = (src.h - ch) / 2;
    Image crop;
    crop.w = cw;
    crop.h = ch;
    crop.opaque = src.opaque;
    crop.px.resize((size_t)cw * ch);
    for (int y = 0; y < ch; ++y)
        std::copy_n(src.px.data() + (size_t)(y + cy) * src.w + cx, cw, crop.px.data() + (size_t)y * cw);
    return scale_image(crop, w, h);
}

bool encode_jpeg(const Image& image, int quality, std::string& out) {
    out.clear();
    if (image.empty()) return false;
    std::vector<unsigned char> rgb((size_t)image.w * image.h * 3);
    for (size_t i = 0; i < image.px.size(); ++i) {
        const uint32_t p = image.px[i];
        // Transparent parts over white, as covers are shown on light cards.
        const uint32_t a = p >> 24;
        const uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        rgb[i * 3] = (unsigned char)((r * a + 255 * (255 - a)) / 255);
        rgb[i * 3 + 1] = (unsigned char)((g * a + 255 * (255 - a)) / 255);
        rgb[i * 3 + 2] = (unsigned char)((b * a + 255 * (255 - a)) / 255);
    }
    auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size);
    };
    return stbi_write_jpg_to_func(append, &out, image.w, image.h, 3, rgb.data(), quality) != 0 && !out.empty();
}

}  // namespace gfx
