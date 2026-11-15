// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "gfx/font.h"

#include <math.h>
#include <pthread.h>

#include <memory>
#include <unordered_map>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace gfx {

namespace {

struct FontFile {
    std::string data;
    stbtt_fontinfo info{};
    bool ok = false;
};

struct Glyph {
    int advance = 0;  // 26.6 fixed point would be nicer; whole pixels keep layout simple
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<uint8_t> mask;
    bool rendered = false;
};

const char* kFiles[(int)Face::Count] = {
    "NotoSans-Regular.ttf", "NotoSans-SemiBold.ttf",   "NotoSans-Bold.ttf",
    "NotoSans-Italic.ttf",  "NotoSerif-Regular.ttf",   "NotoSerif-Bold.ttf",
    "NotoSerif-Italic.ttf", "NotoSerif-BoldItalic.ttf", "NotoSansMono-Regular.ttf",
};

FontFile g_fonts[(int)Face::Count];
// Japanese (kana and kanji) when a face has no glyph: the interface in Japanese.
FontFile g_cjk;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<uint64_t, Glyph> g_glyphs;
std::unordered_map<uint64_t, int> g_kerning;

// Which loaded font actually has this code point: the face itself, then the
// plain sans and serif faces as fallbacks.
const FontFile* font_for(Face face, uint32_t cp, int& glyph_index) {
    const Face order[] = {face, Face::Sans, Face::Serif, Face::Mono};
    for (Face f : order) {
        const FontFile& font = g_fonts[(int)f];
        if (!font.ok) continue;
        glyph_index = stbtt_FindGlyphIndex(&font.info, (int)cp);
        if (glyph_index) return &font;
    }
    if (g_cjk.ok && (glyph_index = stbtt_FindGlyphIndex(&g_cjk.info, (int)cp)) != 0) return &g_cjk;
    const FontFile& font = g_fonts[(int)face].ok ? g_fonts[(int)face] : g_fonts[0];
    glyph_index = 0;  // .notdef box
    return font.ok ? &font : nullptr;
}

uint64_t key(Face face, int px, uint32_t cp) { return ((uint64_t)face << 56) | ((uint64_t)px << 32) | cp; }

// Caller holds g_lock.
Glyph& glyph(Face face, int px, uint32_t cp, bool need_bitmap) {
    if (g_glyphs.size() > 12000) g_glyphs.clear();
    Glyph& g = g_glyphs[key(face, px, cp)];
    if (g.advance == 0 && !g.rendered) {
        int index = 0;
        const FontFile* font = font_for(face, cp, index);
        if (font) {
            const float scale = stbtt_ScaleForMappingEmToPixels(&font->info, (float)px);
            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font->info, index, &advance, &lsb);
            g.advance = (int)lroundf(advance * scale);
            if (g.advance == 0) g.advance = -1;  // marks "measured" for zero-width marks
        }
    }
    if (need_bitmap && !g.rendered) {
        g.rendered = true;
        int index = 0;
        const FontFile* font = font_for(face, cp, index);
        if (font && cp > 32) {
            const float scale = stbtt_ScaleForMappingEmToPixels(&font->info, (float)px);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&font->info, index, scale, scale, &x0, &y0, &x1, &y1);
            g.x0 = x0;
            g.y0 = y0;
            g.w = x1 - x0;
            g.h = y1 - y0;
            if (g.w > 0 && g.h > 0) {
                g.mask.resize((size_t)g.w * g.h);
                stbtt_MakeGlyphBitmap(&font->info, g.mask.data(), g.w, g.h, g.w, scale, scale, index);
            }
        }
    }
    return g;
}

int kern(Face face, int px, uint32_t a, uint32_t b) {
    const FontFile& font = g_fonts[(int)face];
    if (!font.ok || a > 0xFFFF || b > 0xFFFF) return 0;
    const uint64_t k = ((uint64_t)face << 60) | ((uint64_t)(px & 0xFFF) << 48) | ((uint64_t)a << 16) | b;
    auto found = g_kerning.find(k);
    if (found != g_kerning.end()) return found->second;
    if (g_kerning.size() > 50000) g_kerning.clear();
    const float scale = stbtt_ScaleForMappingEmToPixels(&font.info, (float)px);
    const int value = (int)lroundf(stbtt_GetCodepointKernAdvance(&font.info, (int)a, (int)b) * scale);
    g_kerning[k] = value;
    return value;
}

int advance_of(const Glyph& g) { return g.advance < 0 ? 0 : g.advance; }

}  // namespace

bool fonts_init(const std::string& directory) {
    bool all = true;
    for (int i = 0; i < (int)Face::Count; ++i) {
        FontFile& f = g_fonts[i];
        if (!lib::read_file(lib::join_path(directory, kFiles[i]), f.data)) {
            LOG("font missing: %s", kFiles[i]);
            all = false;
            continue;
        }
        f.ok = stbtt_InitFont(&f.info, (const unsigned char*)f.data.data(), 0) != 0;
        if (!f.ok) all = false;
    }
    // Optional: without it, Japanese shows as boxes.
    if (lib::read_file(lib::join_path(directory, "NotoSansJP-Regular.otf"), g_cjk.data))
        g_cjk.ok = stbtt_InitFont(&g_cjk.info, (const unsigned char*)g_cjk.data.data(), 0) != 0;
    if (!g_cjk.ok) LOG("font: no Japanese fallback (NotoSansJP-Regular.otf)");
    return g_fonts[0].ok && all;
}

namespace {

// Text is laid out in logical pixels (gfx/canvas.h) and rasterised at the
// screen's own size: `px` logical is real_px(px) real pixels.
int real_px(int px) {
    const float s = render_scale();
    return s == 1.f ? px : std::max(1, (int)lroundf(px * s));
}

FontMetrics real_metrics(Face face, int px) {
    const FontFile& f = g_fonts[(int)face].ok ? g_fonts[(int)face] : g_fonts[0];
    FontMetrics m;
    if (!f.ok) return m;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, (float)px);
    m.ascent = (int)ceilf(ascent * scale);
    m.descent = (int)ceilf(-descent * scale);
    m.line_height = m.ascent + m.descent + (int)lroundf(gap * scale);
    return m;
}

int to_logical(int v) {
    const float s = render_scale();
    return s == 1.f ? v : (int)lroundf(v / s);
}

}  // namespace

FontMetrics font_metrics(Face face, int px) {
    FontMetrics m = real_metrics(face, real_px(px));
    if (render_scale() == 1.f) return m;
    m.ascent = to_logical(m.ascent);
    m.descent = to_logical(m.descent);
    m.line_height = to_logical(m.line_height);
    return m;
}

int text_width(Face face, int lpx, std::string_view text) {
    const int px = real_px(lpx);
    int width = 0;
    uint32_t previous = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = lib::utf8_next(text, i);
        if (previous) width += kern(face, px, previous, cp);
        width += advance_of(glyph(face, px, cp, false));
        previous = cp;
    }
    pthread_mutex_unlock(&g_lock);
    return to_logical(width);
}

namespace {

// In real pixels; returns the advance in real pixels.
int draw_real(Canvas& canvas, Face face, int px, int x, int baseline, std::string_view text, Color color) {
    const int start = x;
    uint32_t previous = 0;
    const Rect& clip = canvas.clip();
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = lib::utf8_next(text, i);
        if (cp == 0xAD) continue;  // soft hyphen: invisible unless a line breaks there
        if (previous) x += kern(face, px, previous, cp);
        previous = cp;
        if (x > clip.right()) {
            // Still count the advance for the return value.
            x += advance_of(glyph(face, px, cp, false));
            continue;
        }
        const Glyph& g = glyph(face, px, cp, true);
        if (!g.mask.empty()) canvas.blend_mask(x + g.x0, baseline + g.y0, g.mask.data(), g.w, g.h, g.w, color);
        x += advance_of(g);
    }
    pthread_mutex_unlock(&g_lock);
    return x - start;
}

}  // namespace

int draw_text_baseline(Canvas& canvas, Face face, int px, int x, int baseline, std::string_view text, Color color) {
    return to_logical(draw_real(canvas, face, real_px(px), canvas.px(x), canvas.px(baseline), text, color));
}

int draw_text(Canvas& canvas, Face face, int px, int x, int y, std::string_view text, Color color) {
    const int rp = real_px(px);
    return to_logical(draw_real(canvas, face, rp, canvas.px(x), canvas.px(y) + real_metrics(face, rp).ascent, text, color));
}

std::string ellipsize(Face face, int px, std::string_view text, int max_width) {
    if (text_width(face, px, text) <= max_width) return std::string(text);
    const char* dots = "\xE2\x80\xA6";
    const int dots_w = text_width(face, px, dots);
    // Binary search over code point counts.
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const size_t bytes = lib::utf8_prefix(text, mid);
        if (text_width(face, px, text.substr(0, bytes)) + dots_w <= max_width) lo = mid;
        else hi = mid - 1;
    }
    return lib::trim(text.substr(0, lib::utf8_prefix(text, lo))) + dots;
}

namespace {

// Japanese and Chinese have no spaces: a line may break between any two of
// their characters (but not before closing punctuation such as 、。」).
bool cjk(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}
bool closing(uint32_t cp) {
    return cp == 0x3001 || cp == 0x3002 || cp == 0x300D || cp == 0x300F || cp == 0xFF09 || cp == 0xFF0C ||
           cp == 0xFF0E || cp == 0x30FC || cp == 0xFF01 || cp == 0xFF1F || cp == 0x3011;
}

struct Token {
    std::string text;
    bool glued = false;  // no space before it
};

std::vector<Token> tokens(std::string_view text) {
    std::vector<Token> out;
    for (const std::string& word : lib::split(lib::collapse_space(text), ' ')) {
        if (word.empty()) continue;
        bool first = true;
        std::string run;
        size_t i = 0;
        auto flush = [&] {
            if (run.empty()) return;
            out.push_back({run, !first});
            first = false;
            run.clear();
        };
        while (i < word.size()) {
            const size_t start = i;
            const uint32_t cp = lib::utf8_next(word, i);
            const std::string ch = word.substr(start, i - start);
            if (closing(cp) && run.empty() && !out.empty() && !first) {
                out.back().text += ch;  // stays with what it closes
            } else if (cjk(cp) && !closing(cp)) {
                flush();
                run = ch;
                flush();
            } else if (closing(cp)) {
                run += ch;
                flush();
            } else {
                run += ch;  // Latin text inside a Japanese sentence stays one piece
            }
        }
        flush();
    }
    return out;
}

}  // namespace

std::vector<std::string> wrap_text(Face face, int px, std::string_view text, int width, int max_lines) {
    std::vector<std::string> lines;
    const auto words = tokens(text);
    std::string line;
    const int space = text_width(face, px, " ");
    int line_w = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& word = words[i].text;
        const int gap = words[i].glued ? 0 : space;
        const int ww = text_width(face, px, word);
        if (!line.empty() && line_w + gap + ww > width) {
            lines.push_back(line);
            line.clear();
            line_w = 0;
            if ((int)lines.size() == max_lines) {
                // Put the rest on the last line, ellipsized.
                std::string rest = lines.back();
                for (size_t j = i; j < words.size(); ++j) rest += (words[j].glued ? "" : " ") + words[j].text;
                lines.back() = ellipsize(face, px, rest, width);
                return lines;
            }
        }
        if (!line.empty()) {
            if (!words[i].glued) line += ' ';
            line_w += gap;
        }
        line += word;
        line_w += ww;
    }
    if (!line.empty()) lines.push_back(width > 0 ? ellipsize(face, px, line, width) : line);
    return lines;
}

int draw_wrapped(Canvas& canvas, Face face, int px, int x, int y, int width, std::string_view text, Color color,
                 int max_lines, int line_height) {
    if (line_height <= 0) line_height = font_metrics(face, px).line_height;
    const auto lines = wrap_text(face, px, text, width, max_lines);
    for (size_t i = 0; i < lines.size(); ++i) draw_text(canvas, face, px, x, y + (int)i * line_height, lines[i], color);
    return (int)lines.size() * line_height;
}

}  // namespace gfx
