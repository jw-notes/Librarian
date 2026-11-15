// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/fb2.h"

#include <ctype.h>

#include <functional>

#include "core/fs.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "doc/archive.h"
#include "doc/html_convert.h"

namespace doc {

namespace {

// Windows-1251 (Cyrillic), 0x80..0xBF; 0xC0..0xFF are А..я in order.
const uint16_t kCp1251High[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B,
    0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C,
    0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC,
    0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB,
    0x0458, 0x0405, 0x0455, 0x0457};

// KOI8-R letters 0xC0..0xFF.
const char* kKoi8Letters = "юабцдефгхийклмнопярстужвьызшэщчъЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ";

std::string escape(const std::string& s) { return lib::escape_xml(s); }

}  // namespace

std::string decode_legacy(const std::string& bytes, const std::string& encoding) {
    const std::string enc = lib::lower(encoding);
    if (enc.empty() || enc == "utf-8" || enc == "utf8") return lib::to_utf8(bytes);
    std::string out;
    out.reserve(bytes.size() * 2);
    if (enc == "windows-1251" || enc == "cp1251") {
        for (unsigned char c : bytes) {
            if (c < 0x80) out += (char)c;
            else if (c < 0xC0) lib::utf8_append(out, kCp1251High[c - 0x80]);
            else lib::utf8_append(out, 0x0410 + (c - 0xC0));
        }
        return out;
    }
    if (enc == "koi8-r") {
        std::vector<uint32_t> letters;
        for (size_t i = 0; kKoi8Letters[i];) letters.push_back(lib::utf8_next(kKoi8Letters, i));
        for (unsigned char c : bytes) {
            if (c < 0x80) out += (char)c;
            else if (c >= 0xC0) lib::utf8_append(out, letters[c - 0xC0]);
            else if (c == 0xA3) lib::utf8_append(out, 0x0451);
            else if (c == 0xB3) lib::utf8_append(out, 0x0401);
            else lib::utf8_append(out, 0xFFFD);
        }
        return out;
    }
    // windows-1252, iso-8859-1 and anything unknown.
    return lib::to_utf8(bytes);
}

std::string base64_decode(const std::string& text) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;  // whitespace, padding
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

std::string Fb2::image_key(const std::string& id) const { return "fb2:" + path_ + '\x01' + id; }

int Fb2::section_of(const std::string& id) const {
    auto found = ids_.find(id);
    return found == ids_.end() ? -1 : found->second;
}

bool Fb2::open(const std::string& path, std::string& error) {
    *this = Fb2();
    path_ = path;
    std::string raw;
    if (lib::ends_with(lib::lower(path), ".zip") || lib::file_extension(path) == "fbz") {
        auto archive = open_archive(path, &error);
        if (!archive) return false;
        std::string inner;
        for (const auto& e : archive->entries())
            if (lib::ends_with(lib::lower(e.name), ".fb2")) inner = e.name;
        if (inner.empty() || !archive->read(inner, raw)) {
            error = "The archive contains no .fb2 book.";
            return false;
        }
    } else if (!lib::read_file(path, raw, 256ull * 1024 * 1024)) {
        error = "The file could not be read.";
        return false;
    }
    // The encoding named in the XML declaration.
    std::string encoding;
    const size_t decl = raw.find("encoding=");
    if (decl != std::string::npos && decl < 200) {
        const size_t q = decl + 9;
        const char quote = q < raw.size() ? raw[q] : '"';
        const size_t end = raw.find(quote, q + 1);
        if (end != std::string::npos) encoding = raw.substr(q + 1, end - q - 1);
    }
    auto root = lib::parse_markup(decode_legacy(raw, encoding), lib::MarkupMode::Xml);
    const lib::Node* book = root->find("fictionbook");
    if (!book) {
        error = "This is not a FictionBook (FB2) file.";
        return false;
    }

    // Metadata.
    if (const lib::Node* info = book->find("title-info")) {
        title_ = info->child_text("book-title");
        for (const lib::Node* a : info->elements("author")) {
            std::string name = lib::collapse_space(a->child_text("first-name") + " " + a->child_text("middle-name") + " " +
                                                   a->child_text("last-name"));
            if (name.empty()) name = a->child_text("nickname");
            if (!name.empty()) author_ += (author_.empty() ? "" : ", ") + name;
        }
        if (const lib::Node* ann = info->child("annotation")) annotation_ = ann->inner_text();
        if (const lib::Node* cover = info->child("coverpage"))
            if (const lib::Node* img = cover->find("image")) {
                std::string href = img->attr("href");
                if (!href.empty() && href[0] == '#') href.erase(0, 1);
                cover_ = image_key(href);
            }
    }

    // Pictures.
    for (const lib::Node* bin : book->elements("binary")) images_[image_key(bin->attr("id"))] = base64_decode(bin->inner_text());

    // The text: each top-level section of the main body is a reader section;
    // footnotes ("notes" bodies) follow as one section.
    std::string html;
    std::function<void(const lib::Node&, int)> emit_node;
    // The children of `n`.
    std::function<void(const lib::Node&, int)> emit = [&](const lib::Node& n, int depth) {
        for (const auto& child : n.children) emit_node(*child, depth);
    };
    // One node and what it contains.
    emit_node = [&](const lib::Node& c, int depth) {
        if (c.is_text()) {
            html += escape(c.text);
            return;
        }
        const std::string_view t = c.local();
        const std::string id = c.attr("id");
        const std::string id_attr = id.empty() ? "" : " id=\"" + escape(id) + "\"";
        if (!id.empty()) ids_[id] = (int)sections_.size();
        if (t == "section") {
            html += "<div" + id_attr + ">";
            emit(c, depth + 1);
            html += "</div>";
        } else if (t == "title") {
            const int level = std::min(6, depth + 1);
            std::string text;
            for (const lib::Node* p : c.elements("p")) text += (text.empty() ? "" : " ") + p->inner_text();
            if (text.empty()) text = c.inner_text();
            html += "<h" + std::to_string(level) + id_attr + ">";
            bool first = true;
            for (const lib::Node* p : c.elements("p")) {
                if (!first) html += "<br/>";
                first = false;
                emit(*p, depth);
            }
            if (first) emit(c, depth);
            html += "</h" + std::to_string(level) + ">";
            if (!text.empty() && depth >= 1)
                toc_.push_back({text, (int)sections_.size(), id.empty() ? "" : id, depth});
        } else if (t == "p") {
            html += "<p" + id_attr + ">";
            emit(c, depth);
            html += "</p>";
        } else if (t == "subtitle") {
            html += "<h5" + id_attr + ">";
            emit(c, depth);
            html += "</h5>";
        } else if (t == "empty-line") {
            html += "<p>&#160;</p>";
        } else if (t == "epigraph" || t == "cite" || t == "annotation") {
            html += "<blockquote" + id_attr + ">";
            emit(c, depth);
            html += "</blockquote>";
        } else if (t == "text-author") {
            html += "<p><i>";
            emit(c, depth);
            html += "</i></p>";
        } else if (t == "poem" || t == "stanza") {
            html += "<div" + id_attr + ">";
            emit(c, depth);
            html += "</div>";
        } else if (t == "v") {
            html += "<p>";
            emit(c, depth);
            html += "</p>";
        } else if (t == "image") {
            std::string href = c.attr("href");
            if (!href.empty() && href[0] == '#') href.erase(0, 1);
            html += "<img src=\"" + escape(image_key(href)) + "\" alt=\"" + escape(c.attr("alt")) + "\"/>";
        } else if (t == "emphasis") {
            html += "<em>";
            emit(c, depth);
            html += "</em>";
        } else if (t == "strong") {
            html += "<b>";
            emit(c, depth);
            html += "</b>";
        } else if (t == "strikethrough") {
            html += "<s>";
            emit(c, depth);
            html += "</s>";
        } else if (t == "sup" || t == "sub" || t == "code" || t == "table" || t == "tr" || t == "td" || t == "th") {
            const std::string tag(t);
            html += "<" + tag + ">";
            emit(c, depth);
            html += "</" + tag + ">";
        } else if (t == "a") {
            const bool note = c.attr("type") == "note";
            html += std::string(note ? "<sup>" : "") + "<a href=\"" + escape(c.attr("href")) + "\">";
            emit(c, depth);
            html += std::string("</a>") + (note ? "</sup>" : "");
        } else {
            emit(c, depth);
        }
    };
    auto flush = [&] {
        if (!lib::trim(html).empty()) sections_.push_back(html);
        html.clear();
    };
    for (const lib::Node* body : book->elements("body")) {
        const bool notes = !body->attr("name").empty();
        if (notes) {
            flush();
            html += "<h1>" + std::string(body->attr("name") == "notes" ? "Notes" : "Comments") + "</h1>";
            toc_.push_back({body->attr("name") == "notes" ? "Notes" : "Comments", (int)sections_.size(), "", 1});
            emit(*body, 1);
            flush();
            continue;
        }
        // Content before the first section (the book's title page) joins the first section.
        for (const auto& child : body->children) {
            if (!child->is_text() && child->local() == "section") {
                flush();
                if (!child->child("title")) {
                    const lib::Node* paragraph = child->find("p");
                    std::string label = paragraph ? lib::collapse_space(paragraph->inner_text()) : "";
                    if (label.empty()) label = "Illustration";
                    if (label.size() > 100) label = label.substr(0, lib::utf8_prefix(label, 80)) + "...";
                    toc_.push_back({label, (int)sections_.size(), child->attr("id"), 1});
                }
                html += "<div" + (child->attr("id").empty() ? std::string() : " id=\"" + escape(child->attr("id")) + "\"") + ">";
                if (!child->attr("id").empty()) ids_[child->attr("id")] = (int)sections_.size();
                emit(*child, 1);
                html += "</div>";
                flush();
            } else {
                emit_node(*child, 1);
            }
        }
        flush();
    }
    if (sections_.empty()) {
        error = "The book contains no text.";
        return false;
    }
    if (title_.empty()) title_ = lib::file_stem(path);
    LOG("fb2: %zu sections, %zu pictures", sections_.size(), images_.size());
    return true;
}

bool Fb2::load_section(int index, Document& out, std::string& error) const {
    if (index < 0 || index >= (int)sections_.size()) {
        error = "No such section.";
        return false;
    }
    ConvertOptions options;
    const Fb2* self = this;
    options.resolve = [self](const std::string& href) -> std::string {
        if (!href.empty() && href[0] == '#') {
            const int s = self->section_of(href.substr(1));
            return "fb2:" + std::to_string(s < 0 ? 0 : s) + href;
        }
        return href;
    };
    options.min_image = 8;
    html_to_document(*lib::parse_markup(sections_[index], lib::MarkupMode::Html), options, out);
    if (out.blocks.empty()) {
        Block b;
        b.runs.push_back({" ", 0, -1});
        out.blocks.push_back(b);
    }
    return true;
}

}  // namespace doc
