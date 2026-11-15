// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/table.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"

namespace doc {

std::string_view Table::cell(int row, int column) const {
    if (row < 0 || row >= rows() || column < 0 || column >= row_length(row)) return {};
    const uint32_t i = row_start_[row] + column;
    return std::string_view(text_).substr(cell_start_[i], cell_start_[i + 1] - cell_start_[i]);
}

int Table::row_length(int row) const {
    if (row < 0 || row >= rows()) return 0;
    const uint32_t end = row + 1 < rows() ? row_start_[row + 1] : (uint32_t)cell_start_.size() - 1;
    return (int)(end - row_start_[row]);
}

void Table::add_cell(std::string_view text) {
    if (row_cells_ == 0) row_start_.push_back((uint32_t)cell_start_.size());
    if ((int)row_cells_ >= kTableMaxColumns) return;
    cell_start_.push_back((uint32_t)text_.size());
    text_.append(text);
    ++row_cells_;
}

void Table::end_row() {
    columns_ = std::max(columns_, (int)row_cells_);
    row_cells_ = 0;
}

void Table::finish() {
    if (row_cells_) end_row();
    cell_start_.push_back((uint32_t)text_.size());
    text_.shrink_to_fit();
    cell_start_.shrink_to_fit();
    row_start_.shrink_to_fit();
    // Numbers by column, from the rows after the first (a sample is enough).
    numeric_.assign(columns_, false);
    std::vector<int> numbers(columns_, 0), filled(columns_, 0);
    const int last = std::min(rows(), 2000);
    for (int r = 1; r < last; ++r)
        for (int c = 0; c < row_length(r); ++c) {
            const std::string_view v = cell(r, c);
            if (v.empty()) continue;
            ++filled[c];
            if (looks_numeric(v)) ++numbers[c];
        }
    for (int c = 0; c < columns_; ++c) numeric_[c] = filled[c] > 0 && numbers[c] * 10 >= filled[c] * 8;
    // A header: the first row is all text, every cell filled, and at least one
    // column below it holds numbers, or the first row repeats nowhere below.
    if (rows() >= 2) {
        bool all_text = row_length(0) > 0, any_numeric = false;
        for (int c = 0; c < row_length(0); ++c) {
            const std::string_view v = cell(0, c);
            if (v.empty() || looks_numeric(v)) all_text = false;
            if (numeric_[c]) any_numeric = true;
        }
        bool distinct = true;
        if (all_text && !any_numeric) {
            // Text everywhere: a header if its values do not occur in their columns below.
            for (int c = 0; c < row_length(0) && distinct; ++c)
                for (int r = 1; r < last; ++r)
                    if (cell(r, c) == cell(0, c)) {
                        distinct = false;
                        break;
                    }
        }
        header = all_text && (any_numeric || distinct);
    }
}

bool looks_numeric(std::string_view v) {
    std::string_view s = v;
    while (!s.empty() && isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && isspace((unsigned char)s.back())) s.remove_suffix(1);
    if (s.empty()) return false;
    // Currency signs and a percent sign around a number still make a number.
    static const char* const kAffixes[] = {"$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5"};
    for (const char* a : kAffixes)
        if (s.substr(0, strlen(a)) == a) s.remove_prefix(strlen(a));
    if (!s.empty() && s.back() == '%') s.remove_suffix(1);
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) s.remove_prefix(1);
    if (!s.empty() && s.front() == '(' && s.back() == ')') s = s.substr(1, s.size() - 2);  // (12.50)
    int digits = 0;
    bool exp = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (isdigit((unsigned char)ch)) ++digits;
        else if (ch == '.' || ch == ',' || ch == ' ' || ch == '\'') continue;  // separators
        else if ((ch == 'e' || ch == 'E') && digits && !exp && i + 1 < s.size()) {
            exp = true;
            if (s[i + 1] == '-' || s[i + 1] == '+') ++i;
        } else return false;
    }
    return digits > 0;
}

char guess_delimiter(std::string_view text) {
    // The candidate that splits the first lines into the same, largest number
    // of fields (quoted text ignored).
    const char candidates[] = {',', ';', '\t', '|'};
    char best = ',';
    int best_score = -1;
    for (char d : candidates) {
        std::vector<int> counts;
        int n = 0;
        bool quoted = false;
        for (size_t i = 0; i < text.size() && counts.size() < 20; ++i) {
            const char ch = text[i];
            if (ch == '"') quoted = !quoted;
            else if (!quoted && ch == d) ++n;
            else if (!quoted && (ch == '\n' || ch == '\r')) {
                if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                counts.push_back(n);
                n = 0;
            }
        }
        if (n) counts.push_back(n);
        if (counts.empty() || counts[0] == 0) continue;
        int same = 0;
        for (int c : counts) same += c == counts[0];
        // Consistency first, then more columns.
        const int score = same * 1000 + counts[0];
        if (score > best_score) {
            best_score = score;
            best = d;
        }
    }
    return best;
}

namespace {

// UTF-16 (with a BOM) to UTF-8.
std::string from_utf16(std::string_view b, bool big_endian) {
    std::string out;
    out.reserve(b.size() / 2 + 16);
    for (size_t i = 2; i + 1 < b.size(); i += 2) {
        uint32_t u = big_endian ? ((uint8_t)b[i] << 8) | (uint8_t)b[i + 1] : ((uint8_t)b[i + 1] << 8) | (uint8_t)b[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
            const uint32_t lo = big_endian ? ((uint8_t)b[i + 2] << 8) | (uint8_t)b[i + 3]
                                           : ((uint8_t)b[i + 3] << 8) | (uint8_t)b[i + 2];
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        lib::utf8_append(out, u);
    }
    return out;
}

}  // namespace

Table parse_delimited(std::string_view bytes, char delimiter) {
    std::string converted;
    std::string_view text = bytes;
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE) {
        converted = from_utf16(bytes, false);
        text = converted;
    } else if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFE && (uint8_t)bytes[1] == 0xFF) {
        converted = from_utf16(bytes, true);
        text = converted;
    } else {
        if (bytes.size() >= 3 && !memcmp(bytes.data(), "\xEF\xBB\xBF", 3)) text.remove_prefix(3);
        if (!lib::is_valid_utf8(text)) {
            converted = lib::to_utf8(text);
            text = converted;
        }
    }
    Table t;
    t.delimiter = delimiter ? delimiter : guess_delimiter(text);
    const char d = t.delimiter;
    std::string field;
    bool quoted = false, was_quoted = false, row_open = false;
    size_t i = 0;
    for (; i < text.size(); ++i) {
        const char ch = text[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else quoted = false;
            } else field += ch;
            continue;
        }
        if (ch == '"' && (field.empty() && !was_quoted)) {
            quoted = was_quoted = true;
            row_open = true;
        } else if (ch == d) {
            t.add_cell(field);
            field.clear();
            was_quoted = false;
            row_open = true;
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (row_open || !field.empty()) {
                t.add_cell(field);
                t.end_row();
            }
            field.clear();
            was_quoted = row_open = false;
            if (t.rows() >= kTableMaxRows) {
                t.truncated = i + 1 < text.size();
                break;
            }
        } else {
            field += ch;
            row_open = true;
        }
    }
    if (i >= text.size() && (row_open || !field.empty())) {
        t.add_cell(field);
        t.end_row();
    }
    t.finish();
    return t;
}

bool is_table_format(const std::string& format) { return format == "csv" || format == "tsv" || format == "tab"; }

bool open_table(const std::string& path, Table& out, std::string& error) {
    if (lib::file_size(path) > kTableMaxBytes) {
        error = "This table is larger than " + std::to_string(kTableMaxBytes >> 20) +
                " MB, more than Librarian can open.";
        return false;
    }
    std::string bytes;
    if (!lib::read_file(path, bytes, kTableMaxBytes)) {
        error = "The file could not be read.";
        return false;
    }
    const std::string ext = lib::file_extension(path);
    out = parse_delimited(bytes, ext == "tsv" || ext == "tab" ? '\t' : 0);
    bytes = std::string();
    if (out.rows() == 0) {
        error = "The table is empty.";
        return false;
    }
    LOG("table: %s: %d rows, %d columns, delimiter '%s'%s", path.c_str(), out.rows(), out.columns(),
        out.delimiter == '\t' ? "tab" : std::string(1, out.delimiter).c_str(), out.truncated ? ", truncated" : "");
    return true;
}

}  // namespace doc
