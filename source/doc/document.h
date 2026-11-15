// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// A reflowable document: what Kiwix articles, EPUB chapters and plain-text
// files are turned into before layout. It keeps the structure that matters
// for reading (headings, paragraphs, lists, quotes, code, images, tables,
// links) and none of the source's styling, so text size, fonts, spacing and
// colours are the reader's choice.
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace doc {

enum Style : uint16_t {
    kBold = 1,
    kItalic = 2,
    kMono = 4,
    kSuper = 8,
    kSub = 16,
    kUnderline = 32,
    kStrike = 64,
    kSmall = 128,
};

struct Run {
    std::string text;  // "\n" is a forced line break
    uint16_t style = 0;
    int link = -1;  // index into Document::links
    int mark = -1;  // index into Document::marks (read-along: a narrated passage)
};

enum class BlockKind { Paragraph, Heading, ListItem, Preformatted, Image, Rule, Table, Caption };
enum class Align { Start, Center, End };

struct Cell {
    std::vector<Run> runs;
    bool header = false;
    int colspan = 1;
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    std::vector<Run> runs;
    int level = 0;     // heading level 1..6
    int indent = 0;    // list / quote / definition nesting, in steps
    bool quote = false;
    Align align = Align::Start;
    std::string marker;  // list marker: "•", "3.", "a."
    // Images
    std::string src;     // resolved source (URL or archive path)
    int width = 0, height = 0;  // intrinsic or declared size, 0 if unknown
    std::string alt;
    int link = -1;  // clickable image, index into Document::links
    // Tables
    std::vector<std::vector<Cell>> rows;
};

struct TocEntry {
    std::string title;
    int block = 0;
    int level = 1;
};

struct Document {
    std::string title;
    std::vector<Block> blocks;
    std::vector<std::string> links;     // resolved targets
    std::map<std::string, int> anchors;  // element id -> block index
    std::vector<TocEntry> toc;
    // Read-along (EPUB 3 media overlays, DAISY): the narrated passages, by the
    // id of their element. Their text carries the mark, so the reader can
    // highlight exactly that passage. mark_block: where each one starts.
    std::vector<std::string> marks;
    std::map<std::string, int> mark_of;
    std::vector<int> mark_block;

    int add_link(const std::string& href) {
        links.push_back(href);
        return (int)links.size() - 1;
    }
    size_t text_size() const;
};

// Splits plain text into paragraphs (blank lines separate them; single line
// breaks inside a paragraph are joined, as hard-wrapped text files expect).
Document text_document(const std::string& text, const std::string& title);

}  // namespace doc
