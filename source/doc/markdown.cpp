// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/markdown.h"

#include <ctype.h>

#include <vector>

#include "core/text.h"

namespace doc {

namespace {

std::string esc(const std::string& s) { return lib::escape_xml(s); }

// Expands tabs to four spaces and strips a trailing '\r'.
std::string clean(const std::string& line) {
    std::string out;
    for (char c : line) {
        if (c == '\r') continue;
        if (c == '\t') out.append(4 - out.size() % 4, ' ');
        else out += c;
    }
    return out;
}

int indent_of(const std::string& s) {
    int n = 0;
    while (n < (int)s.size() && s[n] == ' ') ++n;
    return n;
}

bool blank(const std::string& s) { return lib::trim(s).empty(); }

// ---- footnotes -------------------------------------------------------------------------

// The document's footnotes ([^name]: text), numbered in the order they are
// defined. Set while one document is converted.
std::vector<std::pair<std::string, std::string>>* g_footnotes = nullptr;

int footnote_number(const std::string& name) {
    if (!g_footnotes) return 0;
    for (size_t i = 0; i < g_footnotes->size(); ++i)
        if ((*g_footnotes)[i].first == name) return (int)i + 1;
    return 0;
}

// ---- inline ------------------------------------------------------------------------

// Emphasis / code / links / images within one run of text.
std::string inline_md(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        // Escaped character.
        if (c == '\\' && i + 1 < s.size() && !isalnum((unsigned char)s[i + 1])) {
            out += esc(std::string(1, s[i + 1]));
            i += 2;
            continue;
        }
        // Inline code `...` (no processing inside).
        if (c == '`') {
            size_t ticks = 0;
            while (i + ticks < s.size() && s[i + ticks] == '`') ++ticks;
            const std::string fence(ticks, '`');
            const size_t end = s.find(fence, i + ticks);
            if (end != std::string::npos) {
                std::string code = s.substr(i + ticks, end - i - ticks);
                code = lib::trim(code);
                out += "<code>" + esc(code) + "</code>";
                i = end + ticks;
                continue;
            }
        }
        // Footnote reference [^name]: a raised number linking to its note.
        if (c == '[' && i + 1 < s.size() && s[i + 1] == '^') {
            const size_t close = s.find(']', i + 2);
            if (close != std::string::npos && close > i + 2) {
                const std::string name = s.substr(i + 2, close - i - 2);
                const int number = footnote_number(name);
                if (number > 0) {
                    const std::string n = std::to_string(number);
                    out += "<sup id=\"fnref-" + esc(name) + "\"><a href=\"#fn-" + esc(name) + "\">" + n + "</a></sup>";
                    i = close + 1;
                    continue;
                }
            }
        }
        // Image ![alt](url) or link [text](url).
        if ((c == '!' && i + 1 < s.size() && s[i + 1] == '[') || c == '[') {
            const bool image = c == '!';
            const size_t open = i + (image ? 1 : 0);  // position of '['
            const size_t close = s.find(']', open + 1);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                const size_t paren = s.find(')', close + 2);
                if (paren != std::string::npos) {
                    const std::string label = s.substr(open + 1, close - open - 1);
                    std::string dest = lib::trim(s.substr(close + 2, paren - close - 2));
                    // A "url title" form: drop the title.
                    const size_t sp = dest.find(' ');
                    if (sp != std::string::npos) dest = dest.substr(0, sp);
                    if (image) out += "<img src=\"" + esc(dest) + "\" alt=\"" + esc(label) + "\"/>";
                    else out += "<a href=\"" + esc(dest) + "\">" + inline_md(label) + "</a>";
                    i = paren + 1;
                    continue;
                }
            }
        }
        // Autolink <http://...>.
        if (c == '<') {
            const size_t end = s.find('>', i + 1);
            if (end != std::string::npos) {
                const std::string inner = s.substr(i + 1, end - i - 1);
                if (lib::starts_with(inner, "http://") || lib::starts_with(inner, "https://")) {
                    out += "<a href=\"" + esc(inner) + "\">" + esc(inner) + "</a>";
                    i = end + 1;
                    continue;
                }
            }
        }
        // Strong / emphasis with * or _.
        if (c == '*' || c == '_') {
            const bool dbl = i + 1 < s.size() && s[i + 1] == c;
            const std::string marker(dbl ? 2 : 1, c);
            const size_t end = s.find(marker, i + marker.size());
            // '_' only delimits at word boundaries (so file_name_here is literal).
            const bool ok = end != std::string::npos && end > i + marker.size() &&
                            (c == '*' || ((i == 0 || !isalnum((unsigned char)s[i - 1])) &&
                                          (end + marker.size() >= s.size() || !isalnum((unsigned char)s[end + marker.size()]))));
            if (ok) {
                const std::string inner = inline_md(s.substr(i + marker.size(), end - i - marker.size()));
                out += dbl ? "<b>" + inner + "</b>" : "<i>" + inner + "</i>";
                i = end + marker.size();
                continue;
            }
        }
        // Strikethrough ~~...~~.
        if (c == '~' && i + 1 < s.size() && s[i + 1] == '~') {
            const size_t end = s.find("~~", i + 2);
            if (end != std::string::npos) {
                out += "<s>" + inline_md(s.substr(i + 2, end - i - 2)) + "</s>";
                i = end + 2;
                continue;
            }
        }
        // Hard line break: two or more trailing spaces before a newline were
        // turned into a marker by the block parser; here just pass text.
        out += esc(std::string(1, c));
        ++i;
    }
    return out;
}

bool is_rule(const std::string& s) {
    const std::string t = lib::trim(s);
    if (t.size() < 3) return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char ch : t)
        if (ch != c && ch != ' ') return false;
    return true;
}

// A table separator row: | --- | :--: | ...
bool is_table_sep(const std::string& s) {
    const std::string t = lib::trim(s);
    if (t.find('|') == std::string::npos && t.find('-') == std::string::npos) return false;
    bool dash = false;
    for (char c : t) {
        if (c == '-') dash = true;
        else if (c != '|' && c != ':' && c != ' ') return false;
    }
    return dash;
}

std::vector<std::string> table_cells(const std::string& row) {
    std::string r = lib::trim(row);
    if (!r.empty() && r.front() == '|') r.erase(0, 1);
    if (!r.empty() && r.back() == '|') r.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] == '\\' && i + 1 < r.size()) {
            cur += r[i];
            cur += r[i + 1];
            ++i;
        } else if (r[i] == '|') {
            cells.push_back(lib::trim(cur));
            cur.clear();
        } else {
            cur += r[i];
        }
    }
    cells.push_back(lib::trim(cur));
    return cells;
}

class Parser {
public:
    explicit Parser(const std::vector<std::string>& lines) : lines_(lines) {}

    std::string run() {
        while (i_ < lines_.size()) block();
        return out_;
    }

private:
    void block() {
        const std::string& line = lines_[i_];
        if (blank(line)) {
            ++i_;
            return;
        }
        const std::string t = lib::trim(line);
        // Fenced code.
        if (lib::starts_with(t, "```") || lib::starts_with(t, "~~~")) {
            fenced_code(t.substr(0, 3));
            return;
        }
        // Indented code (4 spaces), only outside lists.
        if (indent_of(line) >= 4) {
            indented_code();
            return;
        }
        // ATX heading.
        if (t[0] == '#') {
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            if (h >= 1 && h <= 6 && (h == t.size() || t[h] == ' ')) {
                std::string text = lib::trim(t.substr(h));
                while (!text.empty() && text.back() == '#') text.pop_back();
                out_ += "<h" + std::to_string(h) + ">" + inline_md(lib::trim(text)) + "</h" + std::to_string(h) + ">\n";
                ++i_;
                return;
            }
        }
        if (is_rule(line)) {
            out_ += "<hr/>\n";
            ++i_;
            return;
        }
        if (t[0] == '>') {
            blockquote();
            return;
        }
        if (is_list_item(line)) {
            list(indent_of(line));
            return;
        }
        // Table: a header row followed by a separator row.
        if (line.find('|') != std::string::npos && i_ + 1 < lines_.size() && is_table_sep(lines_[i_ + 1])) {
            table();
            return;
        }
        paragraph();
    }

    void fenced_code(const std::string& fence) {
        ++i_;
        std::string code;
        while (i_ < lines_.size() && lib::trim(lines_[i_]).substr(0, 3) != fence) {
            code += lines_[i_] + "\n";
            ++i_;
        }
        if (i_ < lines_.size()) ++i_;  // closing fence
        out_ += "<pre>" + esc(code) + "</pre>\n";
    }

    void indented_code() {
        std::string code;
        while (i_ < lines_.size() && (indent_of(lines_[i_]) >= 4 || blank(lines_[i_]))) {
            if (blank(lines_[i_]) && (i_ + 1 >= lines_.size() || indent_of(lines_[i_ + 1]) < 4)) break;
            code += (lines_[i_].size() >= 4 ? lines_[i_].substr(4) : std::string()) + "\n";
            ++i_;
        }
        out_ += "<pre>" + esc(code) + "</pre>\n";
    }

    void blockquote() {
        std::vector<std::string> inner;
        while (i_ < lines_.size() && !blank(lines_[i_]) && lib::trim(lines_[i_])[0] == '>') {
            std::string s = lib::trim(lines_[i_]);
            s.erase(0, 1);
            if (!s.empty() && s[0] == ' ') s.erase(0, 1);
            inner.push_back(s);
            ++i_;
        }
        out_ += "<blockquote>\n" + Parser(inner).run() + "</blockquote>\n";
    }

    static bool is_list_item(const std::string& line) {
        const std::string t = line.substr(indent_of(line));
        if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') return true;
        size_t d = 0;
        while (d < t.size() && isdigit((unsigned char)t[d])) ++d;
        return d > 0 && d < t.size() && (t[d] == '.' || t[d] == ')') && d + 1 < t.size() && t[d + 1] == ' ';
    }

    static bool ordered(const std::string& line) {
        const std::string t = line.substr(indent_of(line));
        return !t.empty() && isdigit((unsigned char)t[0]);
    }

    // Returns the item's content after its marker.
    static std::string item_content(const std::string& line, int& marker_width) {
        const int ind = indent_of(line);
        const std::string t = line.substr(ind);
        size_t m = 0;
        if (t[0] == '-' || t[0] == '*' || t[0] == '+') m = 1;
        else {
            while (m < t.size() && isdigit((unsigned char)t[m])) ++m;
            ++m;  // the . or )
        }
        while (m < t.size() && t[m] == ' ') ++m;
        marker_width = ind + (int)m;
        return t.substr(m);
    }

    void list(int base_indent) {
        const bool ord = ordered(lines_[i_]);
        out_ += ord ? "<ol>\n" : "<ul>\n";
        while (i_ < lines_.size()) {
            if (blank(lines_[i_])) {
                // A blank line ends the list unless the next line continues it.
                if (i_ + 1 < lines_.size() && is_list_item(lines_[i_ + 1]) && indent_of(lines_[i_ + 1]) >= base_indent) {
                    ++i_;
                    continue;
                }
                break;
            }
            if (!is_list_item(lines_[i_]) || indent_of(lines_[i_]) < base_indent) break;
            if (indent_of(lines_[i_]) == base_indent && ordered(lines_[i_]) != ord) break;  // a list of the other kind
            if (indent_of(lines_[i_]) > base_indent + 1) {
                // A nested list under the current item.
                list(indent_of(lines_[i_]));
                continue;
            }
            int marker_width = 0;
            std::vector<std::string> body;
            body.push_back(item_content(lines_[i_], marker_width));
            ++i_;
            // Continuation and nested lines belong to this item.
            while (i_ < lines_.size() && !blank(lines_[i_]) && !is_list_item(lines_[i_]) &&
                   indent_of(lines_[i_]) >= marker_width) {
                body.push_back(lines_[i_].substr(std::min((int)lines_[i_].size(), marker_width)));
                ++i_;
            }
            std::vector<std::string> nested;
            while (i_ < lines_.size() && is_list_item(lines_[i_]) && indent_of(lines_[i_]) > base_indent + 1) {
                nested.push_back(lines_[i_]);
                ++i_;
            }
            // A task list item: "[ ] to do" / "[x] done".
            const std::string first = lib::trim(body[0]);
            if (!ord && first.size() >= 3 && first[0] == '[' && first[2] == ']' &&
                (first[1] == ' ' || first[1] == 'x' || first[1] == 'X') && (first.size() == 3 || first[3] == ' ')) {
                // □ (open) or ✓ (done): both in the fonts Librarian ships.
                body[0] = std::string(first[1] == ' ' ? "\xE2\x96\xA1" : "\xE2\x9C\x93") + first.substr(3);
            }
            // A tight item (one line, nothing nested) is inline, with no <p>.
            if (body.size() == 1 && nested.empty()) {
                out_ += "<li>" + inline_md(lib::trim(body[0])) + "</li>\n";
            } else {
                out_ += "<li>" + Parser(body).run();
                if (!nested.empty()) out_ += Parser(nested).run();
                out_ += "</li>\n";
            }
        }
        out_ += ord ? "</ol>\n" : "</ul>\n";
    }

    void table() {
        const auto header = table_cells(lines_[i_]);
        i_ += 2;  // header + separator
        out_ += "<table>\n<tr>";
        for (const auto& c : header) out_ += "<th>" + inline_md(c) + "</th>";
        out_ += "</tr>\n";
        while (i_ < lines_.size() && !blank(lines_[i_]) && lines_[i_].find('|') != std::string::npos) {
            out_ += "<tr>";
            for (const auto& c : table_cells(lines_[i_])) out_ += "<td>" + inline_md(c) + "</td>";
            out_ += "</tr>\n";
            ++i_;
        }
        out_ += "</table>\n";
    }

    void paragraph() {
        std::string html;
        while (i_ < lines_.size() && !blank(lines_[i_])) {
            const std::string& line = lines_[i_];
            const std::string t = lib::trim(line);
            // These start a new block.
            if (t[0] == '#' || t[0] == '>' || is_rule(line) || is_list_item(line) ||
                lib::starts_with(t, "```") || lib::starts_with(t, "~~~"))
                break;
            // A setext heading underline turns the paragraph so far into a heading.
            if (!html.empty() && (t.find_first_not_of('=') == std::string::npos ||
                                  t.find_first_not_of('-') == std::string::npos) &&
                !t.empty()) {
                const int level = t[0] == '=' ? 1 : 2;
                out_ += "<h" + std::to_string(level) + ">" + html + "</h" + std::to_string(level) + ">\n";
                ++i_;
                return;
            }
            // Two trailing spaces: a hard line break.
            const bool br = line.size() >= 2 && line[line.size() - 1] == ' ' && line[line.size() - 2] == ' ';
            if (!html.empty()) html += br ? "<br/>" : " ";
            html += inline_md(t);
            ++i_;
        }
        if (!html.empty()) out_ += "<p>" + html + "</p>\n";
    }

    const std::vector<std::string>& lines_;
    size_t i_ = 0;
    std::string out_;
};

}  // namespace

std::string markdown_to_html(const std::string& markdown) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= markdown.size()) {
        size_t nl = markdown.find('\n', start);
        if (nl == std::string::npos) nl = markdown.size();
        lines.push_back(clean(markdown.substr(start, nl - start)));
        start = nl + 1;
    }
    // YAML front matter (---, "key: value" lines, then --- or ...): not
    // shown; its title becomes the heading when the text has none of its own.
    std::string front_title;
    if (!lines.empty() && lib::trim(lines[0]) == "---") {
        for (size_t end = 1; end < lines.size() && end < 200; ++end) {
            const std::string t = lib::trim(lines[end]);
            if (t == "---" || t == "...") {
                for (size_t k = 1; k < end; ++k) {
                    const std::string kv = lib::trim(lines[k]);
                    if (!lib::starts_with(kv, "title:")) continue;
                    front_title = lib::trim(kv.substr(6));
                    if (front_title.size() >= 2 && (front_title[0] == '"' || front_title[0] == '\'') &&
                        front_title.back() == front_title[0])
                        front_title = front_title.substr(1, front_title.size() - 2);
                }
                lines.erase(lines.begin(), lines.begin() + (long)end + 1);
                break;
            }
            // Not front matter after all (a rule, then ordinary text).
            if (!t.empty() && t.find(':') == std::string::npos && t[0] != '-' && t[0] != '#' && lines[end][0] != ' ') break;
        }
    }
    // Footnote definitions "[^name]: text" (indented lines continue them).
    std::vector<std::pair<std::string, std::string>> notes;
    std::vector<std::string> body;
    for (size_t k = 0; k < lines.size(); ++k) {
        const std::string& l = lines[k];
        const size_t close = l.find("]:");
        if (lib::starts_with(l, "[^") && close != std::string::npos && close > 2) {
            std::string text = lib::trim(l.substr(close + 2));
            while (k + 1 < lines.size() && indent_of(lines[k + 1]) >= 4 && !blank(lines[k + 1]))
                text += " " + lib::trim(lines[++k]);
            notes.push_back({l.substr(2, close - 2), text});
            continue;
        }
        body.push_back(l);
    }
    g_footnotes = &notes;
    std::string html;
    if (!front_title.empty()) {
        bool heading = false;
        for (const auto& l : body)
            if (!blank(l)) {
                heading = lib::trim(l)[0] == '#';
                break;
            }
        if (!heading) html += "<h1>" + esc(front_title) + "</h1>\n";
    }
    html += Parser(body).run();
    if (!notes.empty()) {
        // Each note links back to where it is referred to (↩).
        html += "<hr/>\n<h2>Notes</h2>\n";
        for (size_t k = 0; k < notes.size(); ++k)
            html += "<p id=\"fn-" + esc(notes[k].first) + "\">" + std::to_string(k + 1) + ". " + inline_md(notes[k].second) +
                    " <a href=\"#fnref-" + esc(notes[k].first) + "\">\xE2\x86\xA9</a></p>\n";
    }
    g_footnotes = nullptr;
    return html;
}

}  // namespace doc
