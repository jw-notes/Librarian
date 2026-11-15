// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Word-processor documents: DOCX (Office Open XML), ODT (OpenDocument Text)
// and RTF. Each is converted once, when it is opened, into HTML sections that
// the shared HTML converter and the reflowable reader show (as Markdown is):
// headings, paragraphs, lists, tables, pictures, links, footnotes and basic
// character formatting survive; page layout, fonts and colours do not.
//
// The XML is read with a streaming parser (no tree of the whole document),
// and pictures stay in the file until the reader shows them: DOCX and ODT
// pictures through the "archive:" image source, RTF pictures through
// "rtf:" sources that read their bytes back from the file.
#include <stdint.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace doc {

struct HtmlBook {
    std::string title, author;
    std::vector<std::string> sections;  // HTML fragments, about 100 KB each
    struct TocEntry {
        std::string title, anchor;
        int section = 0, level = 1;
    };
    std::vector<TocEntry> toc;
    std::map<std::string, int> anchors;  // element id -> section
    std::string cover;                   // image-cache source, may be empty
};

bool is_office_format(const std::string& format);  // "docx", "odt", "rtf"
// Converts the document (by extension). Runs on any thread.
bool open_office(const std::string& path, HtmlBook& out, std::string& error);
bool open_docx(const std::string& path, HtmlBook& out, std::string& error);
bool open_odt(const std::string& path, HtmlBook& out, std::string& error);
bool open_rtf(const std::string& path, HtmlBook& out, std::string& error);
// Title and author only (document properties), without converting the text.
bool office_metadata(const std::string& path, std::string& title, std::string& author);
// The document's own preview picture (ODT thumbnail, DOCX docProps/thumbnail).
bool office_cover(const std::string& path, std::string& bytes);
// The image-cache loader for "rtf:" sources (once, at startup).
void register_rtf_images();
// RTF \info title and author, from the start of the file.
bool rtf_info(const std::string& path, std::string& title, std::string& author);
// Decodes an RTF picture's hex digits (whitespace between them is ignored).
std::string rtf_hex_decode(std::string_view hex);

// ---- building blocks, shared by the converters and used by the tests -----------------------

// HTML split into sections at block boundaries, so a long document lays out a
// section at a time. A cut is only made outside lists and tables.
class HtmlSections {
public:
    std::string& html() { return current_; }
    // Call between top-level blocks. `heading`: a heading of this level comes next.
    void boundary(int heading = 0);
    int section() const { return (int)done_.size(); }
    void finish(std::vector<std::string>& out);
    size_t target = 100 * 1024;

private:
    std::string current_;
    std::vector<std::string> done_;
};

// A small pull parser for well-formed XML: elements, attributes and text, no
// tree. Comments, processing instructions and DOCTYPE are skipped; CDATA is text.
class XmlPull {
public:
    enum Event { Start, End, Text, Eof };
    explicit XmlPull(std::string_view source) : s_(source) {}
    Event next();
    // Qualified name of the element just started or ended ("w:p").
    std::string_view name() const { return name_; }
    // The local part ("p").
    std::string_view local() const;
    // A decoded attribute value of the element just started, by qualified name.
    std::string attr(std::string_view qname) const;
    bool has_attr(std::string_view qname) const;
    bool self_closing() const { return self_closing_; }
    const std::string& text() const { return text_; }  // decoded
    int depth() const { return depth_; }  // of the element just started / ended
    // Skips to the end of the element just started.
    void skip();

private:
    std::string_view s_;
    size_t pos_ = 0;
    std::string_view name_;
    std::vector<std::pair<std::string_view, std::string_view>> attrs_;
    std::string text_;
    bool self_closing_ = false, pending_end_ = false;
    int depth_ = 0, level_ = 0;
};

// Plain-text helpers for the converters.
std::string html_escape(std::string_view text);

}  // namespace doc
