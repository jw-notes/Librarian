// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Tables: CSV and TSV files read into a grid of text cells (ui/table_viewer.cpp
// shows them). Cells are kept in one text buffer with offsets, so a large
// table costs little more than its file.
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace doc {

class Table {
public:
    int rows() const { return (int)row_start_.size(); }
    int columns() const { return columns_; }
    // "" for a cell past the end of a short row.
    std::string_view cell(int row, int column) const;
    // Rows with fewer cells than the widest one keep their own length here.
    int row_length(int row) const;
    // Whether the first row names the columns (a guess the reader can change).
    bool header = false;
    // Most of a column's cells are numbers: shown right-aligned.
    bool numeric(int column) const { return column < (int)numeric_.size() && numeric_[column]; }
    char delimiter = ',';
    // Rows left out because the file was larger than Librarian keeps.
    bool truncated = false;

    // Builders (doc/table.cpp).
    void add_cell(std::string_view text);
    void end_row();
    void finish();

private:
    std::string text_;
    std::vector<uint32_t> cell_start_;  // offset of each cell in text_ (plus one past the last)
    std::vector<uint32_t> row_start_;   // index of each row's first cell
    std::vector<bool> numeric_;
    int columns_ = 0;
    uint32_t row_cells_ = 0;
};

// Limits: rows past kTableMaxRows are left out (truncated = true); a file
// over kTableMaxBytes is refused (the console has ~400 MB free, and parsing
// holds the file and its cells at once).
constexpr int kTableMaxRows = 200000;
constexpr int kTableMaxColumns = 512;
constexpr uint64_t kTableMaxBytes = 24ull << 20;

// Parses delimited text (RFC 4180 quoting; CRLF, LF or CR line ends). The
// bytes may be UTF-8 (with or without a BOM), UTF-16 with a BOM (Excel's
// "Unicode text"), or Windows-1252. `delimiter` 0 = guess from the first lines.
Table parse_delimited(std::string_view bytes, char delimiter = 0);
// Reads a .csv / .tsv / .tab file; false (and `error`) if it cannot be read.
bool open_table(const std::string& path, Table& out, std::string& error);
bool is_table_format(const std::string& format);  // "csv", "tsv", "tab"

// Exposed for the tests.
char guess_delimiter(std::string_view text);
bool looks_numeric(std::string_view cell);

}  // namespace doc
