// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Lays a Document out for a given text size and column width. The result is
// a list of positioned fragments (text, images, boxes) sorted by their top
// edge, plus the places a page may break and where each block starts, which
// is what reading positions are stored against.
#include <stdint.h>

#include <string>
#include <vector>

#include "doc/document.h"
#include "gfx/font.h"

namespace doc {

struct ReaderStyle {
    int font_px = 34;
    int line_spacing = 150;  // percent of the font size
    bool serif = true;
    bool justify = false;
    int width = 1240;       // column width
    int viewport_h = 940;   // images are kept shorter than this
    bool operator==(const ReaderStyle& o) const {
        return font_px == o.font_px && line_spacing == o.line_spacing && serif == o.serif && justify == o.justify &&
               width == o.width && viewport_h == o.viewport_h;
    }
    bool operator!=(const ReaderStyle& o) const { return !(*this == o); }
};

// What a fragment is drawn with; the reader maps roles to theme colours.
enum class Role : uint8_t { Body, Heading, Muted, Link, Code, Rule, CodeBackground, QuoteBar, TableBorder, TableHeader, Marker };

struct Frag {
    enum Kind : uint8_t { Text, Image, Box } kind = Text;
    Role role = Role::Body;
    gfx::Face face = gfx::Face::Serif;
    uint8_t decoration = 0;  // kUnderline | kStrike
    int px = 0;
    int x = 0, y = 0, w = 0, h = 0;
    int baseline = 0;
    int link = -1;
    int mark = -1;     // read-along passage (Run::mark), highlighted while narrated
    std::string text;  // Text: the string; Image: the source
};

struct Layout {
    std::vector<Frag> frags;
    std::vector<int> breaks;     // y positions where a page may start, ascending
    std::vector<int> block_top;  // per document block
    int height = 0;
    int tallest = 0;  // tallest fragment, for culling

    // Index of the first fragment that may be visible at or below `y`.
    size_t first_visible(int y) const;
    int block_at(int y) const;
    int link_at(int x, int y) const;
    // The y where each page starts, for pages `page_h` tall.
    std::vector<int> page_starts(int page_h) const;
};

Layout layout_document(const Document& doc, const ReaderStyle& style);

}  // namespace doc
