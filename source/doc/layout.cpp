// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/layout.h"

#include <algorithm>
#include <functional>

#include "core/text.h"

namespace doc {

using gfx::Face;

namespace {

// A piece of text with one style: words are made of one or more atoms.
struct Atom {
    std::string text;
    Face face;
    int px;
    int width;
    int link;
    uint16_t style;
    bool space_before;  // a break opportunity precedes this atom
    bool newline;       // forced line break before this atom
    int mark = -1;      // read-along passage (Run::mark)
};

struct TextOptions {
    bool sans = false;     // heading / table / caption text
    bool bold = false;
    bool italic = false;
    int px = 0;
    Role role = Role::Body;
    Align align = Align::Start;
    bool justify = false;
    bool preserve = false;  // preformatted: keep spaces, break only at newlines
};

class Engine {
public:
    Engine(const Document& doc, const ReaderStyle& style) : doc_(doc), s_(style) {}

    Layout run() {
        out_.block_top.resize(doc_.blocks.size(), 0);
        y_ = 0;
        for (size_t i = 0; i < doc_.blocks.size(); ++i) {
            out_.block_top[i] = y_;
            block(doc_.blocks[i], i);
        }
        out_.height = y_ + s_.font_px;
        std::stable_sort(out_.frags.begin(), out_.frags.end(), [](const Frag& a, const Frag& b) { return a.y < b.y; });
        for (const auto& f : out_.frags) out_.tallest = std::max(out_.tallest, f.h);
        std::sort(out_.breaks.begin(), out_.breaks.end());
        out_.breaks.erase(std::unique(out_.breaks.begin(), out_.breaks.end()), out_.breaks.end());
        return std::move(out_);
    }

private:
    int em() const { return s_.font_px; }
    int indent_px(int steps) const { return steps * em() * 3 / 2; }

    Face face_for(uint16_t style, bool sans, bool bold, bool italic) const {
        if (style & kMono) return Face::Mono;
        bold = bold || (style & kBold);
        italic = italic || (style & kItalic);
        if (sans) return bold ? Face::SansBold : italic ? Face::SansItalic : Face::Sans;
        if (!s_.serif) return bold ? Face::SansBold : italic ? Face::SansItalic : Face::Sans;
        if (bold && italic) return Face::SerifBoldItalic;
        return bold ? Face::SerifBold : italic ? Face::SerifItalic : Face::Serif;
    }

    int px_for(uint16_t style, int base) const {
        if (style & (kSuper | kSub)) return std::max(12, base * 7 / 10);
        if (style & kSmall) return std::max(12, base * 85 / 100);
        if (style & kMono) return std::max(12, base * 90 / 100);
        return base;
    }

    std::vector<Atom> atoms_for(const std::vector<Run>& runs, const TextOptions& o) const {
        std::vector<Atom> atoms;
        bool pending_space = false, pending_newline = false;
        for (const Run& run : runs) {
            const Face face = face_for(run.style, o.sans, o.bold, o.italic);
            const int px = px_for(run.style, o.px);
            if (run.text == "\n") {
                pending_newline = true;
                continue;
            }
            size_t i = 0;
            const std::string& t = run.text;
            while (i < t.size()) {
                if (t[i] == '\n') {
                    pending_newline = true;
                    ++i;
                    continue;
                }
                if (t[i] == ' ' && !o.preserve) {
                    pending_space = true;
                    ++i;
                    continue;
                }
                size_t end = i;
                if (o.preserve) {
                    end = t.find('\n', i);
                    if (end == std::string::npos) end = t.size();
                } else {
                    while (end < t.size() && t[end] != ' ' && t[end] != '\n') ++end;
                }
                std::string piece = t.substr(i, end - i);
                // Soft hyphens are invisible; this layout does not hyphenate.
                piece = lib::replace_all(piece, "\xC2\xAD", "");
                if (!piece.empty()) {
                    atoms.push_back({piece, face, px, gfx::text_width(face, px, piece), run.link, run.style,
                                     pending_space, pending_newline, run.mark});
                }
                pending_space = pending_newline = false;
                i = end;
            }
        }
        return atoms;
    }

    // Splits an over-long atom (a URL, a long word in a narrow column) into
    // pieces that fit `width`.
    void split_long(std::vector<Atom>& atoms, int width) const {
        std::vector<Atom> out;
        for (Atom& a : atoms) {
            if (a.width <= width || width <= 0) {
                out.push_back(std::move(a));
                continue;
            }
            size_t i = 0;
            bool first = true;
            while (i < a.text.size()) {
                size_t n = 1, best = lib::utf8_prefix(std::string_view(a.text).substr(i), 1);
                for (;;) {
                    const size_t bytes = lib::utf8_prefix(std::string_view(a.text).substr(i), n + 1);
                    if (i + bytes > a.text.size() || bytes == best) break;
                    if (gfx::text_width(a.face, a.px, a.text.substr(i, bytes)) > width) break;
                    best = bytes;
                    ++n;
                }
                Atom piece = a;
                piece.text = a.text.substr(i, best);
                piece.width = gfx::text_width(a.face, a.px, piece.text);
                piece.space_before = first ? a.space_before : true;
                piece.newline = first ? a.newline : false;
                out.push_back(std::move(piece));
                first = false;
                i += best;
            }
        }
        atoms.swap(out);
    }

    // Lays out `runs` from (x, y) in a column `width` wide; returns the new y.
    int text(const std::vector<Run>& runs, int x, int width, int y, const TextOptions& o,
              std::vector<Frag>* sink = nullptr) {
        std::vector<Frag>& frags = sink ? *sink : out_.frags;
        std::vector<Atom> atoms = atoms_for(runs, o);
        split_long(atoms, width);
        if (atoms.empty()) return y;

        // Group atoms into unbreakable words.
        struct Word { size_t first, last; int width; bool space_before, newline; };
        std::vector<Word> words;
        for (size_t i = 0; i < atoms.size(); ++i) {
            if (words.empty() || atoms[i].space_before || atoms[i].newline || o.preserve) {
                words.push_back({i, i, atoms[i].width, atoms[i].space_before, atoms[i].newline});
            } else {
                words.back().last = i;
                words.back().width += atoms[i].width;
            }
        }
        const Face base_face = face_for(0, o.sans, o.bold, o.italic);
        const int space_w = gfx::text_width(base_face, o.px, " ");
        const gfx::FontMetrics base = gfx::font_metrics(base_face, o.px);
        const int line_h = std::max(base.ascent + base.descent, o.px * s_.line_spacing / 100);

        size_t w = 0;
        while (w < words.size()) {
            // Fill one line.
            size_t end = w;
            int used = words[w].width;
            while (end + 1 < words.size() && !words[end + 1].newline &&
                   used + (words[end + 1].space_before ? space_w : 0) + words[end + 1].width <= width) {
                ++end;
                used += (words[end].space_before ? space_w : 0) + words[end].width;
            }
            const bool last_line = end + 1 >= words.size() || words[end + 1].newline;
            int cx = x;
            int gap_extra = 0, gap_rem = 0, gaps = 0;
            for (size_t k = w + 1; k <= end; ++k) gaps += words[k].space_before;
            if (o.align == Align::Center) cx = x + (width - used) / 2;
            else if (o.align == Align::End) cx = x + width - used;
            else if (o.justify && !last_line && gaps > 0 && used > width * 3 / 4) {
                gap_extra = (width - used) / gaps;
                gap_rem = (width - used) % gaps;
            }

            const int top = y;
            const int baseline = top + (line_h - (base.ascent + base.descent)) / 2 + base.ascent;
            out_.breaks.push_back(top);
            Frag* open = nullptr;  // the fragment adjacent atoms merge into
            int gap_index = 0;
            for (size_t k = w; k <= end; ++k) {
                const Word& word = words[k];
                if (k > w && word.space_before) {
                    const int extra = gap_extra + (gap_index++ < gap_rem ? 1 : 0);
                    // Spaces join the open fragment when nothing differs, so a line of
                    // plain text is usually one fragment.
                    if (open && !gap_extra && atoms[word.first].face == open->face && atoms[word.first].px == open->px &&
                        atoms[word.first].link == open->link && atoms[word.first].mark == open->mark &&
                        open->kind == Frag::Text) {
                        open->text += ' ';
                        open->w += space_w;
                    } else {
                        open = nullptr;
                    }
                    cx += space_w + extra;
                }
                for (size_t a = word.first; a <= word.last; ++a) {
                    const Atom& atom = atoms[a];
                    const bool shifted = atom.style & (kSuper | kSub);
                    const Role role = atom.link >= 0 ? Role::Link : (atom.style & kMono) && o.role == Role::Body ? Role::Code : o.role;
                    const uint8_t deco = atom.style & (kUnderline | kStrike);
                    if (open && !shifted && open->face == atom.face && open->px == atom.px && open->link == atom.link &&
                        open->role == role && open->decoration == deco && open->mark == atom.mark &&
                        open->x + open->w == cx) {
                        open->text += atom.text;
                        open->w += atom.width;
                    } else {
                        Frag f;
                        f.kind = Frag::Text;
                        f.role = role;
                        f.face = atom.face;
                        f.px = atom.px;
                        f.decoration = deco;
                        f.x = cx;
                        f.y = top;
                        f.w = atom.width;
                        f.h = line_h;
                        f.baseline = baseline;
                        if (atom.style & kSuper) f.baseline = baseline - o.px * 38 / 100;
                        if (atom.style & kSub) f.baseline = baseline + o.px * 18 / 100;
                        f.link = atom.link;
                        f.mark = atom.mark;
                        f.text = atom.text;
                        frags.push_back(std::move(f));
                        open = shifted ? nullptr : &frags.back();
                    }
                    cx += atom.width;
                }
            }
            y += line_h;
            w = end + 1;
        }
        return y;
    }

    void box(int x, int y, int w, int h, Role role, std::vector<Frag>* sink = nullptr) {
        Frag f;
        f.kind = Frag::Box;
        f.role = role;
        f.x = x;
        f.y = y;
        f.w = w;
        f.h = h;
        (sink ? *sink : out_.frags).push_back(f);
    }

    void block(const Block& b, size_t index) {
        const int left = indent_px(b.indent);
        const int width = std::max(em() * 4, s_.width - left);
        const int gap = em() * 7 / 10;
        const BlockKind previous = index ? doc_.blocks[index - 1].kind : BlockKind::Paragraph;
        switch (b.kind) {
            case BlockKind::Heading: {
                static const int scale[] = {100, 175, 145, 122, 108, 100, 100};
                const int px = em() * scale[std::min(6, std::max(1, b.level))] / 100;
                if (index) y_ += em() * (b.level <= 2 ? 6 : 4) / 10;
                out_.block_top[index] = y_;
                TextOptions o;
                o.sans = true;
                o.bold = true;
                o.px = px;
                o.role = Role::Heading;
                o.align = b.align;
                const int saved = s_.line_spacing;
                s_.line_spacing = std::min(saved, 125);
                y_ = text(b.runs, left, width, y_, o);
                s_.line_spacing = saved;
                if (b.level <= 2) {
                    y_ += em() / 5;
                    box(left, y_, width, std::max(1, em() / 16), Role::Rule);
                }
                y_ += em() * 4 / 10;
                break;
            }
            case BlockKind::Paragraph:
            case BlockKind::ListItem: {
                TextOptions o;
                o.px = em();
                o.align = b.align;
                o.justify = s_.justify;
                const int top = y_;
                if (b.kind == BlockKind::ListItem) {
                    const Face face = face_for(0, false, false, false);
                    const int mw = gfx::text_width(face, em(), b.marker);
                    Frag m;
                    m.kind = Frag::Text;
                    m.role = Role::Marker;
                    m.face = face;
                    m.px = em();
                    m.x = left - mw - em() / 3;
                    m.y = y_;
                    m.w = mw;
                    m.h = em();
                    const gfx::FontMetrics fm = gfx::font_metrics(face, em());
                    const int line_h = std::max(fm.ascent + fm.descent, em() * s_.line_spacing / 100);
                    m.baseline = y_ + (line_h - (fm.ascent + fm.descent)) / 2 + fm.ascent;
                    m.text = b.marker;
                    out_.frags.push_back(m);
                }
                y_ = text(b.runs, left, width, y_, o);
                if (b.quote) box(left - em() * 3 / 4, top, std::max(3, em() / 8), y_ - top, Role::QuoteBar);
                const bool next_item = index + 1 < doc_.blocks.size() && doc_.blocks[index + 1].kind == BlockKind::ListItem;
                y_ += b.kind == BlockKind::ListItem && next_item ? em() / 4 : gap;
                break;
            }
            case BlockKind::Caption: {
                TextOptions o;
                o.sans = true;
                o.px = em() * 80 / 100;
                o.role = Role::Muted;
                o.align = previous == BlockKind::Image ? Align::Center : b.align;
                if (previous == BlockKind::Image) y_ -= gap / 2;
                y_ = text(b.runs, left, width, y_, o);
                y_ += gap;
                break;
            }
            case BlockKind::Preformatted: {
                TextOptions o;
                o.px = em() * 80 / 100;
                o.preserve = true;
                o.role = Role::Code;
                const int pad = em() / 2;
                const int top = y_;
                std::vector<Frag> inner;
                const int bottom = text(b.runs, left + pad, width - 2 * pad, y_ + pad, o, &inner);
                box(left, top, width, bottom - top + pad, Role::CodeBackground);
                for (auto& f : inner) out_.frags.push_back(std::move(f));
                y_ = bottom + pad + gap;
                break;
            }
            case BlockKind::Rule:
                box(left, y_ + em() / 2, width, std::max(1, em() / 16), Role::Rule);
                y_ += em() + gap / 2;
                break;
            case BlockKind::Image:
                image(b, left, width);
                break;
            case BlockKind::Table:
                table(b, left, width);
                break;
        }
    }

    void image(const Block& b, int left, int width) {
        int w = b.width, h = b.height;
        // Page pixels are scaled with the text: 16 CSS pixels to one em.
        const int max_h = s_.viewport_h * 3 / 4;
        if (w > 0 && h > 0) {
            w = w * em() / 18;
            h = h * em() / 18;
        } else {
            w = width * 6 / 10;
            h = w * 2 / 3;
        }
        if (w > width) {
            h = (int)((int64_t)h * width / w);
            w = width;
        }
        if (h > max_h) {
            w = (int)((int64_t)w * max_h / h);
            h = max_h;
        }
        w = std::max(1, w);
        h = std::max(1, h);
        Frag f;
        f.kind = Frag::Image;
        f.link = b.link;
        f.x = left + (width - w) / 2;
        f.y = y_;
        f.w = w;
        f.h = h;
        f.text = b.src;
        out_.breaks.push_back(y_);
        out_.frags.push_back(std::move(f));
        // No break after the image: a caption may tuck up under it, and the
        // next block's lines add their own.
        y_ += h + em() * 7 / 10;
    }

    // Measures a cell's narrowest sensible width (its longest word) and its
    // natural width (longest line without wrapping).
    void measure(const Cell& c, const TextOptions& o, int& min_w, int& max_w) {
        min_w = max_w = 0;
        const auto atoms = atoms_for(c.runs, o);
        const int space_w = gfx::text_width(face_for(0, o.sans, false, false), o.px, " ");
        int word = 0, line = 0;
        for (size_t i = 0; i < atoms.size(); ++i) {
            const Atom& a = atoms[i];
            if (a.newline) {
                max_w = std::max(max_w, line);
                line = 0;
            }
            if (a.space_before || a.newline) {
                min_w = std::max(min_w, word);
                word = 0;
                if (line) line += space_w;
            }
            word += a.width;
            line += a.width;
        }
        min_w = std::max(min_w, word);
        max_w = std::max(max_w, line);
    }

    void table(const Block& b, int left, int width) {
        TextOptions o;
        o.sans = true;
        o.px = em() * 80 / 100;
        const int pad = o.px * 45 / 100;
        size_t cols = 0;
        for (const auto& row : b.rows) {
            size_t n = 0;
            for (const auto& c : row) n += c.colspan;
            cols = std::max(cols, n);
        }
        std::vector<int> col_min(cols, 0), col_max(cols, 0);
        for (const auto& row : b.rows) {
            size_t col = 0;
            for (const auto& c : row) {
                if (col >= cols) break;
                if (c.colspan == 1) {
                    int mn, mx;
                    TextOptions co = o;
                    co.bold = c.header;
                    measure(c, co, mn, mx);
                    col_min[col] = std::max(col_min[col], mn + 2 * pad);
                    col_max[col] = std::max(col_max[col], std::min(width, mx + 2 * pad));
                }
                col += c.colspan;
            }
        }
        int sum_min = 0, sum_max = 0;
        for (size_t i = 0; i < cols; ++i) {
            col_max[i] = std::max(col_max[i], col_min[i]);
            sum_min += col_min[i];
            sum_max += col_max[i];
        }
        if (sum_min > width && cols > 2) {
            stacked_table(b, left, width);
            return;
        }
        std::vector<int> col_w(cols);
        if (sum_max <= width) {
            col_w = col_max;
        } else if (sum_min >= width) {
            for (size_t i = 0; i < cols; ++i) col_w[i] = std::max(pad * 3, col_min[i] * width / std::max(1, sum_min));
        } else {
            const int extra = width - sum_min, spread = std::max(1, sum_max - sum_min);
            for (size_t i = 0; i < cols; ++i) col_w[i] = col_min[i] + (int)((int64_t)extra * (col_max[i] - col_min[i]) / spread);
        }
        int table_w = 0;
        for (int w : col_w) table_w += w;

        const int border = std::max(1, em() / 24);
        box(left, y_, table_w, border, Role::TableBorder);
        out_.breaks.push_back(y_);
        for (const auto& row : b.rows) {
            std::vector<Frag> cells, backgrounds;
            int row_bottom = y_;
            size_t col = 0;
            int x = left;
            for (const auto& c : row) {
                if (col >= cols) break;
                int w = 0;
                for (int k = 0; k < c.colspan && col + k < cols; ++k) w += col_w[col + k];
                TextOptions co = o;
                co.bold = c.header;
                const int bottom = text(c.runs, x + pad, w - 2 * pad, y_ + pad, co, &cells);
                if (c.header) box(x, y_, w, 0, Role::TableHeader, &backgrounds);
                row_bottom = std::max(row_bottom, bottom + pad);
                x += w;
                col += c.colspan;
            }
            for (auto& bg : backgrounds) {
                bg.h = row_bottom - y_;
                out_.frags.push_back(bg);
            }
            for (auto& f : cells) out_.frags.push_back(std::move(f));
            y_ = row_bottom;
            box(left, y_, table_w, border, Role::TableBorder);
            out_.breaks.push_back(y_);
        }
        y_ += em() * 7 / 10;
    }

    // A table too wide for the column: each row becomes a short block of
    // "Header: value" lines.
    void stacked_table(const Block& b, int left, int width) {
        std::vector<std::string> headers;
        size_t first = 0;
        if (!b.rows.empty() && std::all_of(b.rows[0].begin(), b.rows[0].end(), [](const Cell& c) { return c.header; })) {
            for (const auto& c : b.rows[0]) {
                std::string t;
                for (const auto& r : c.runs) t += r.text == "\n" ? " " : r.text;
                headers.push_back(lib::collapse_space(t));
            }
            first = 1;
        }
        TextOptions o;
        o.sans = true;
        o.px = em() * 80 / 100;
        const int border = std::max(1, em() / 24);
        for (size_t r = first; r < b.rows.size(); ++r) {
            box(left, y_, width, border, Role::TableBorder);
            y_ += o.px / 3;
            for (size_t c = 0; c < b.rows[r].size(); ++c) {
                std::vector<Run> runs;
                if (c < headers.size() && !headers[c].empty()) runs.push_back({headers[c] + ": ", kBold, -1});
                for (const auto& run : b.rows[r][c].runs) runs.push_back(run.text == "\n" ? Run{" ", run.style, run.link} : run);
                y_ = text(runs, left, width, y_, o);
            }
            y_ += o.px / 3;
        }
        box(left, y_, width, border, Role::TableBorder);
        y_ += em() * 7 / 10;
    }

    const Document& doc_;
    ReaderStyle s_;
    Layout out_;
    int y_ = 0;
};

}  // namespace

size_t Layout::first_visible(int y) const {
    const int from = y - tallest;
    auto it = std::lower_bound(frags.begin(), frags.end(), from, [](const Frag& f, int v) { return f.y < v; });
    return (size_t)(it - frags.begin());
}

int Layout::block_at(int y) const {
    auto it = std::upper_bound(block_top.begin(), block_top.end(), y);
    return it == block_top.begin() ? 0 : (int)(it - block_top.begin()) - 1;
}

int Layout::link_at(int x, int y) const {
    // A small tolerance makes superscript footnotes practical with an analogue
    // stick. Prefer the nearest actual rectangle when targets are adjacent.
    constexpr int padding = 6;
    int link = -1, nearest = padding * 2 + 1;
    for (size_t i = first_visible(y - padding); i < frags.size() && frags[i].y <= y + padding; ++i) {
        const auto& f = frags[i];
        if (f.link < 0) continue;
        const int dx = std::max({0, f.x - x, x - (f.x + f.w - 1)});
        const int dy = std::max({0, f.y - y, y - (f.y + f.h - 1)});
        if (dx <= padding && dy <= padding && dx + dy < nearest) {
            link = f.link;
            nearest = dx + dy;
        }
    }
    return link;
}

std::vector<int> Layout::page_starts(int page_h) const {
    std::vector<int> pages;
    int start = 0;
    while (start < height) {
        pages.push_back(start);
        const int limit = start + page_h;
        if (limit >= height) break;
        auto it = std::upper_bound(breaks.begin(), breaks.end(), limit);
        int next = it == breaks.begin() ? limit : *(it - 1);
        if (next <= start) next = limit;  // something taller than a page
        start = next;
    }
    if (pages.empty()) pages.push_back(0);
    return pages;
}

Layout layout_document(const Document& doc, const ReaderStyle& style) { return Engine(doc, style).run(); }

}  // namespace doc
