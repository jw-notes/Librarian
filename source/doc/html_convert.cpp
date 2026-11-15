// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/html_convert.h"

#include <ctype.h>
#include <stdlib.h>

#include <algorithm>

#include "core/text.h"
#include "net/url.h"

namespace doc {

namespace {

using lib::Node;

bool one_of(const std::string& tag, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (tag == n) return true;
    return false;
}

bool is_block_tag(const std::string& t) {
    return one_of(t, {"p", "div", "section", "article", "aside", "main", "header", "footer", "body", "html", "h1", "h2",
                      "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd", "blockquote", "pre", "table", "tr",
                      "td", "th", "tbody", "thead", "tfoot", "caption", "figure", "figcaption", "hr", "center",
                      "details", "summary", "address", "nav", "form", "fieldset", "legend", "menu"});
}

// Page furniture that is never part of the text: MediaWiki edit links and
// navigation boxes, Kiwix's collapse controls, reference back-links, and
// anything the page itself hides.
bool skipped(const Node& n) {
    static const char* kTags[] = {"head", "script", "style", "noscript", "template", "nav", "button", "input",
                                  "select", "textarea", "svg", "iframe", "object", "embed", "audio", "video",
                                  "canvas", "map", "link", "meta", "title"};
    for (const char* t : kTags)
        if (n.tag == t) return true;
    static const char* kClasses[] = {"mw-editsection", "navbox", "vertical-navbox", "navbox-styles", "noprint",
                                     "mw-empty-elt", "metadata", "pcs-ref-backlink-container", "pcs-collapse-table-collapse-text", "pcs-collapse-table-collapsed-container",
                                     "pcs-collapse-table-collapsed-bottom", "pcs-section-control", "mbox-small",
                                     "sistersitebox", "portalbox", "pcs-footer", "mw-jump-link", "toc", "printfooter",
                                     "catlinks", "mw-authority-control", "navigation-not-searchable-hidden"};
    if (n.has_attr("class"))
        for (const char* c : kClasses)
            if (n.has_class(c)) return true;
    if (n.attr("role") == "navigation" || n.attr("aria-hidden") == "true") return true;
    // Kiwix's MediaWiki pages hide sections and infoboxes until their scripts
    // expand them ("pcs-" classes); everything else hidden stays hidden.
    if (n.tag != "section" && n.attr("class").find("pcs-") == std::string::npos) {
        const std::string style = lib::lower(n.attr("style"));
        if (style.find("display:none") != std::string::npos || style.find("display: none") != std::string::npos)
            return true;
    }
    return false;
}

int int_attr(const Node& n, const char* name) {
    const std::string& v = n.attr(name);
    return v.empty() ? 0 : atoi(v.c_str());
}

std::string roman(int n, bool upper) {
    static const std::pair<int, const char*> table[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"},
                                                        {90, "xc"},  {50, "l"},   {40, "xl"}, {10, "x"},   {9, "ix"},
                                                        {5, "v"},    {4, "iv"},   {1, "i"}};
    std::string out;
    for (const auto& [value, s] : table)
        while (n >= value) {
            out += s;
            n -= value;
        }
    if (upper)
        for (char& c : out) c = (char)toupper((unsigned char)c);
    return out;
}

std::string list_marker(const std::string& type, int index) {
    if (type == "a") return std::string(1, (char)('a' + (index - 1) % 26)) + ".";
    if (type == "A") return std::string(1, (char)('A' + (index - 1) % 26)) + ".";
    if (type == "i") return roman(index, false) + ".";
    if (type == "I") return roman(index, true) + ".";
    return std::to_string(index) + ".";
}

struct Context {
    uint16_t style = 0;
    int link = -1;
    bool pre = false;
    int mark = -1;  // read-along passage the text belongs to
};

// MediaWiki footnotes in a lettered group arrive as "[lower-alpha 2]": the
// letter is drawn by a CSS counter. Turn them into what a reader would see.
std::string footnote_label(const std::string& text) {
    static const char* kGroups[] = {"lower-alpha", "upper-alpha", "lower-roman", "upper-roman", "lower-greek"};
    if (text.size() < 4 || text.front() != '[' || text.back() != ']') return text;
    for (const char* group : kGroups) {
        const std::string prefix = std::string("[") + group + " ";
        if (!lib::starts_with(text, prefix)) continue;
        const int n = atoi(text.c_str() + prefix.size());
        if (n <= 0) return text;
        const std::string g = group;
        std::string label;
        if (g == "lower-alpha" || g == "upper-alpha") {
            for (int v = n; v > 0; v = (v - 1) / 26) label.insert(label.begin(), (char)((g[0] == 'l' ? 'a' : 'A') + (v - 1) % 26));
        } else if (g == "lower-greek") {
            lib::utf8_append(label, 0x3B1 + (uint32_t)((n - 1) % 24));
        } else {
            label = roman(n, g[0] == 'u');
        }
        return "[" + label + "]";
    }
    return text;
}

class Converter {
public:
    Converter(const ConvertOptions& options, Document& doc) : options_(options), doc_(doc) {}

    void run(const Node& root) {
        const Node* body = root.find("body");
        if (doc_.title.empty())
            if (const Node* title = root.find("title")) doc_.title = title->inner_text();
        walk(body ? *body : root);
        flush();
    }

private:
    // ---- inline content ------------------------------------------------------

    void add_text(const std::string& raw, const Context& ctx) {
        std::string text;
        if (ctx.pre) {
            text = raw;
        } else {
            // Collapse whitespace, keeping one space at a boundary.
            text.reserve(raw.size());
            for (char c : raw) {
                if (!isspace((unsigned char)c)) text += c;
                else if (text.empty() || text.back() != ' ') text += ' ';
            }
            if (!text.empty() && text[0] == ' ' && (current_.runs.empty() || ends_with_space())) text.erase(0, 1);
        }
        if (text.empty()) return;
        if (ctx.style & kSuper) text = footnote_label(text);
        // nbsp is a non-breaking space; layout handles it as part of a word.
        auto& runs = current_.runs;
        if (!runs.empty() && runs.back().style == ctx.style && runs.back().link == ctx.link &&
            runs.back().mark == ctx.mark && runs.back().text != "\n")
            runs.back().text += text;
        else
            runs.push_back({text, ctx.style, ctx.link, ctx.mark});
    }

    bool ends_with_space() const {
        const auto& runs = current_.runs;
        return !runs.empty() && !runs.back().text.empty() &&
               (runs.back().text.back() == ' ' || runs.back().text == "\n");
    }

    bool has_content(const Block& b) const {
        for (const auto& r : b.runs)
            for (char c : r.text)
                if (!isspace((unsigned char)c)) return true;
        return false;
    }

    void flush() {
        if (has_content(current_)) {
            // Trim trailing spaces and line breaks.
            auto& runs = current_.runs;
            while (!runs.empty() && (runs.back().text == "\n" || lib::trim(runs.back().text).empty())) runs.pop_back();
            if (!runs.empty() && current_.kind != BlockKind::Preformatted) {
                while (!runs.back().text.empty() && runs.back().text.back() == ' ') runs.back().text.pop_back();
            }
            if (current_.kind == BlockKind::Heading) {
                std::string title;
                for (const auto& r : runs) title += r.text;
                doc_.toc.push_back({lib::collapse_space(title), (int)doc_.blocks.size(), current_.level});
            }
            push_block(std::move(current_));
        }
        current_ = Block();
        current_.kind = kind_;
        current_.level = level_;
        current_.indent = indent_;
        current_.quote = quote_ > 0;
        current_.align = align_;
    }

    // Every block enters the document here, so ids seen before its content
    // point at it.
    void push_block(Block&& b) {
        for (const auto& id : pending_ids_) doc_.anchors[id] = (int)doc_.blocks.size();
        pending_ids_.clear();
        // Where each read-along passage starts.
        auto note = [&](const std::vector<Run>& runs) {
            for (const Run& r : runs)
                if (r.mark >= 0 && doc_.mark_block[r.mark] < 0) doc_.mark_block[r.mark] = (int)doc_.blocks.size();
        };
        note(b.runs);
        for (const auto& row : b.rows)
            for (const auto& cell : row) note(cell.runs);
        doc_.blocks.push_back(std::move(b));
    }

    void note_anchor(const Node& n) {
        // Kindle (KF8) books also link to elements by their "aid" attribute.
        if (!n.attr("aid").empty()) note_id(n.attr("aid"));
        if (!n.attr("id").empty()) note_id(n.attr("id"));
    }

    void note_id(const std::string& id) {
        // Inside a paragraph that already has text, the id is that paragraph;
        // otherwise it is whatever block comes next.
        if (has_content(current_)) doc_.anchors[id] = (int)doc_.blocks.size();
        else pending_ids_.push_back(id);
    }

    // ---- images --------------------------------------------------------------

    void image(const Node& n, int link = -1) {
        std::string src = n.attr("src");
        if (src.empty()) src = n.attr("href");  // SVG <image xlink:href>
        if (src.empty()) src = n.attr("data-src");
        if (src.empty() || lib::starts_with(src, "data:")) return;
        int w = int_attr(n, "width"), h = int_attr(n, "height");
        const std::string resolved = options_.resolve ? options_.resolve(src) : src;
        if ((w <= 0 || h <= 0) && options_.image_size) options_.image_size(resolved, w, h);
        if (w > 0 && h > 0 && w < options_.min_image && h < options_.min_image) return;
        // Vector images cannot be drawn: keep their description.
        if (lib::ends_with(lib::lower(lib::url_path_only(resolved)), ".svg")) {
            const std::string alt = n.attr("alt");
            if (!alt.empty()) add_text(" " + alt + " ", Context{kItalic, -1, false});
            return;
        }
        if (in_table_) {
            // Pictures inside tables (infobox photos) are shown before the table.
            if (table_images_.size() < 3 && (w == 0 || w >= 80)) {
                Block b;
                b.kind = BlockKind::Image;
                b.link = link;
                b.src = resolved;
                b.width = w;
                b.height = h;
                b.alt = n.attr("alt");
                b.align = Align::Center;
                table_images_.push_back(std::move(b));
            }
            return;
        }
        flush();
        Block b;
        b.kind = BlockKind::Image;
        b.link = link;
        b.src = resolved;
        b.width = w;
        b.height = h;
        b.alt = n.attr("alt");
        b.align = Align::Center;
        b.indent = indent_;
        push_block(std::move(b));
        flush();
    }

    // ---- tables --------------------------------------------------------------

    void collect_cell_runs(const Node& n, Context ctx, std::vector<Run>& out) {
        for (const auto& child : n.children) {
            const Node& c = *child;
            if (c.is_text()) {
                std::string t = lib::collapse_space(c.text);
                if (t.empty()) continue;
                const bool space_before = isspace((unsigned char)c.text.front());
                if (space_before && !out.empty() && out.back().text != "\n" && out.back().text.back() != ' ') t = " " + t;
                if (isspace((unsigned char)c.text.back())) t += " ";
                if (!out.empty() && out.back().style == ctx.style && out.back().link == ctx.link && out.back().text != "\n")
                    out.back().text += t;
                else
                    out.push_back({t, ctx.style, ctx.link});
                continue;
            }
            if (skipped(c)) continue;
            if (c.tag == "img") { image(c, ctx.link); continue; }
            if (c.tag == "br" || c.tag == "p" || c.tag == "div" || c.tag == "li" || c.tag == "tr" || c.tag == "dd" ||
                c.tag == "dt") {
                if (!out.empty() && out.back().text != "\n") out.push_back({"\n", 0, -1});
                if (c.tag == "br") continue;
            }
            Context inner = ctx;
            apply_inline_style(c, inner);
            if (c.tag == "li") out.push_back({"\xE2\x80\xA2 ", inner.style, -1});
            collect_cell_runs(c, inner, out);
            if ((c.tag == "p" || c.tag == "div" || c.tag == "li") && !out.empty() && out.back().text != "\n")
                out.push_back({"\n", 0, -1});
        }
    }

    void table(const Node& n) {
        flush();
        std::vector<const Node*> rows;
        std::function<void(const Node&)> gather = [&](const Node& t) {
            for (const auto& c : t.children) {
                if (c->is_text() || skipped(*c)) continue;
                if (c->tag == "tr") rows.push_back(c.get());
                else if (c->tag == "tbody" || c->tag == "thead" || c->tag == "tfoot") gather(*c);
            }
        };
        gather(n);
        Block b;
        b.kind = BlockKind::Table;
        b.indent = indent_;
        const bool was_in_table = in_table_;
        in_table_ = true;
        std::vector<Block> images;
        table_images_.swap(images);
        size_t max_cols = 0;
        for (const Node* tr : rows) {
            std::vector<Cell> cells;
            for (const auto& c : tr->children) {
                if (c->is_text() || (c->tag != "td" && c->tag != "th") || skipped(*c)) continue;
                Cell cell;
                cell.header = c->tag == "th";
                cell.colspan = std::max(1, std::min(20, int_attr(*c, "colspan")));
                Context ctx;
                if (cell.header) ctx.style |= kBold;
                collect_cell_runs(*c, ctx, cell.runs);
                while (!cell.runs.empty() && (cell.runs.back().text == "\n" || lib::trim(cell.runs.back().text).empty()))
                    cell.runs.pop_back();
                while (!cell.runs.empty() && lib::trim(cell.runs.front().text).empty()) cell.runs.erase(cell.runs.begin());
                cells.push_back(std::move(cell));
            }
            size_t span = 0;
            for (const auto& c : cells) span += c.colspan;
            max_cols = std::max(max_cols, span);
            bool any = false;
            for (const auto& c : cells) any = any || !c.runs.empty();
            if (any) b.rows.push_back(std::move(cells));
        }
        in_table_ = was_in_table;
        table_images_.swap(images);
        for (auto& img : images) {
            img.indent = indent_;
            push_block(std::move(img));
        }
        // Caption above the table.
        if (const Node* caption = n.child("caption")) {
            Block c;
            c.kind = BlockKind::Caption;
            c.indent = indent_;
            c.runs.push_back({caption->inner_text(), kBold, -1});
            if (!c.runs[0].text.empty()) push_block(std::move(c));
        }
        if (b.rows.empty()) {
        } else if (max_cols <= 1) {
            // A one-column table is a box of paragraphs (quote boxes, layout tables).
            for (auto& row : b.rows)
                for (auto& cell : row) {
                    Block p;
                    p.kind = BlockKind::Paragraph;
                    p.indent = indent_;
                    p.runs = std::move(cell.runs);
                    push_block(std::move(p));
                }
        } else {
            push_block(std::move(b));
        }
        flush();
    }

    // ---- elements ------------------------------------------------------------

    void apply_inline_style(const Node& n, Context& ctx) {
        const std::string& t = n.tag;
        if (t == "b" || t == "strong") ctx.style |= kBold;
        else if (t == "i" || t == "em" || t == "cite" || t == "var" || t == "dfn") ctx.style |= kItalic;
        else if (t == "code" || t == "kbd" || t == "samp" || t == "tt") ctx.style |= kMono;
        else if (t == "sup") ctx.style |= kSuper;
        else if (t == "sub") ctx.style |= kSub;
        else if (t == "u" || t == "ins") ctx.style |= kUnderline;
        else if (t == "s" || t == "del" || t == "strike") ctx.style |= kStrike;
        else if (t == "small") ctx.style |= kSmall;
        else if (t == "a") {
            const std::string& href = n.attr("href");
            if (!href.empty() && !lib::starts_with(href, "javascript:"))
                ctx.link = doc_.add_link(options_.resolve ? options_.resolve(href) : href);
        } else if (t == "th") ctx.style |= kBold;
        if (!n.attr("style").empty()) {
            const std::string style = lib::lower(n.attr("style"));
            if (style.find("font-weight:bold") != std::string::npos || style.find("font-weight: bold") != std::string::npos)
                ctx.style |= kBold;
            if (style.find("font-style:italic") != std::string::npos || style.find("font-style: italic") != std::string::npos)
                ctx.style |= kItalic;
        }
    }

    void walk(const Node& n, Context ctx = Context()) {
        for (const auto& child : n.children) {
            const Node& c = *child;
            if (c.is_text()) {
                add_text(c.text, ctx);
                continue;
            }
            // An SVG wrapper around a picture (EPUB cover pages): keep the picture.
            if (c.tag == "svg") {
                if (const Node* picture = c.find("image")) image(*picture);
                continue;
            }
            if (skipped(c)) continue;
            element(c, ctx);
        }
    }

    void with_block(BlockKind kind, int level, const std::function<void()>& body) {
        flush();
        const BlockKind saved_kind = kind_;
        const int saved_level = level_;
        kind_ = kind;
        level_ = level;
        current_.kind = kind;
        current_.level = level;
        body();
        flush();
        kind_ = saved_kind;
        level_ = saved_level;
        current_.kind = kind_;
        current_.level = level_;
    }

    void element(const Node& c, Context ctx) {
        const std::string& t = c.tag;
        // A block element starts a new block: close the open one before its id
        // is recorded, so the id points at the new block.
        if (is_block_tag(t) || t == "img" || t == "hr") flush();
        note_anchor(c);
        if (!options_.mark_ids.empty()) {
            const std::string& id = c.attr("id");
            if (!id.empty() && options_.mark_ids.count(id) && !doc_.mark_of.count(id)) {
                ctx.mark = (int)doc_.marks.size();
                doc_.mark_of[id] = ctx.mark;
                doc_.marks.push_back(id);
                doc_.mark_block.push_back(-1);
            }
        }

        if (t == "img" || t == "image") { image(c, ctx.link); return; }
        if (t == "math") {
            // MathML: Wikipedia supplies a TeX rendering in alttext.
            const std::string alt = c.attr("alttext");
            add_text(alt.empty() ? c.inner_text() : alt, Context{(uint16_t)(ctx.style | kMono), ctx.link, false});
            return;
        }
        if (t == "br") {
            current_.runs.push_back({"\n", ctx.style, -1});
            return;
        }
        if (t == "hr") {
            if (c.attr("id") == "pcs-edit-section-divider") return;
            flush();
            Block b;
            b.kind = BlockKind::Rule;
            push_block(std::move(b));
            flush();
            return;
        }
        if (t == "table") { table(c); return; }
        if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
            with_block(BlockKind::Heading, t[1] - '0', [&] { walk(c, ctx); });
            return;
        }
        if (t == "pre") {
            Context inner = ctx;
            inner.pre = true;
            inner.style |= kMono;
            with_block(BlockKind::Preformatted, 0, [&] { walk(c, inner); });
            return;
        }
        if (t == "ul" || t == "ol" || t == "menu") {
            flush();
            const bool ordered = t == "ol";
            int index = c.has_attr("start") ? int_attr(c, "start") : 1;
            const std::string type = c.attr("type");
            ++indent_;
            for (const auto& li : c.children) {
                if (li->is_text()) {
                    add_text(li->text, ctx);
                    continue;
                }
                if (skipped(*li)) continue;
                if (li->tag != "li") {
                    element(*li, ctx);
                    continue;
                }
                note_anchor(*li);
                const std::string marker = ordered ? list_marker(type, index++) : (indent_ % 2 ? "\xE2\x80\xA2" : "\xE2\x97\xA6");
                with_block(BlockKind::ListItem, 0, [&] {
                    current_.marker = marker;
                    walk(*li, ctx);
                });
            }
            --indent_;
            flush();
            return;
        }
        if (t == "li") {
            with_block(BlockKind::ListItem, 0, [&] {
                current_.marker = "\xE2\x80\xA2";
                walk(c, ctx);
            });
            return;
        }
        if (t == "dd") {
            ++indent_;
            with_block(BlockKind::Paragraph, 0, [&] { walk(c, ctx); });
            --indent_;
            return;
        }
        if (t == "dt") {
            Context inner = ctx;
            inner.style |= kBold;
            with_block(BlockKind::Paragraph, 0, [&] { walk(c, inner); });
            return;
        }
        if (t == "blockquote") {
            ++indent_;
            ++quote_;
            flush();
            walk(c, ctx);
            flush();
            --quote_;
            --indent_;
            flush();
            return;
        }
        if (t == "figcaption" || c.has_class("thumbcaption") || c.has_class("gallerytext")) {
            with_block(BlockKind::Caption, 0, [&] { walk(c, ctx); });
            return;
        }
        if (t == "center" || lib::lower(c.attr("style")).find("text-align:center") != std::string::npos ||
            c.attr("align") == "center") {
            const Align saved = align_;
            align_ = Align::Center;
            if (is_block_tag(t)) {
                flush();
                walk(c, ctx);
                flush();
            } else {
                walk(c, ctx);
            }
            align_ = saved;
            if (is_block_tag(t)) flush();
            return;
        }
        if (c.has_class("hatnote") || c.attr("id") == "pcs-edit-section-title-description") {
            Context inner = ctx;
            inner.style |= kItalic;
            with_block(BlockKind::Paragraph, 0, [&] { walk(c, inner); });
            return;
        }
        if (t == "q") {
            add_text("\xE2\x80\x9C", ctx);
            walk(c, ctx);
            add_text("\xE2\x80\x9D", ctx);
            return;
        }
        if (is_block_tag(t)) {
            flush();
            walk(c, ctx);
            flush();
            return;
        }
        // Inline element.
        Context inner = ctx;
        apply_inline_style(c, inner);
        walk(c, inner);
    }

    const ConvertOptions& options_;
    Document& doc_;
    Block current_;
    BlockKind kind_ = BlockKind::Paragraph;
    int level_ = 0, indent_ = 0, quote_ = 0;
    Align align_ = Align::Start;
    bool in_table_ = false;
    std::vector<Block> table_images_;
    std::vector<std::string> pending_ids_;
};

}  // namespace

void html_to_document(const lib::Node& root, const ConvertOptions& options, Document& out) {
    Converter(options, out).run(root);
}

Document html_to_document(const std::string& html, const ConvertOptions& options) {
    Document d;
    auto root = lib::parse_markup(html, lib::MarkupMode::Html);
    html_to_document(*root, options, d);
    return d;
}

size_t Document::text_size() const {
    size_t n = 0;
    for (const auto& b : blocks) {
        for (const auto& r : b.runs) n += r.text.size();
        for (const auto& row : b.rows)
            for (const auto& c : row)
                for (const auto& r : c.runs) n += r.text.size();
    }
    return n;
}

Document text_document(const std::string& text, const std::string& title) {
    Document d;
    d.title = title;
    std::string paragraph;
    auto flush = [&] {
        const std::string p = lib::trim(paragraph);
        if (!p.empty()) {
            Block b;
            b.runs.push_back({lib::collapse_space(p), 0, -1});
            d.blocks.push_back(std::move(b));
        }
        paragraph.clear();
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lib::trim(line).empty()) flush();
        else paragraph += line + " ";
        start = end + 1;
    }
    flush();
    return d;
}

}  // namespace doc
