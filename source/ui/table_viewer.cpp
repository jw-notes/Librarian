// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The table viewer (CSV, TSV): a grid with a frozen header row and row
// numbers, moved through cell by cell with the D-pad. The selected cell is
// shown in full under the grid; X opens the whole row as a card (every column
// with its name), the way a long row reads best on a TV.
#include <stdlib.h>

#include <algorithm>
#include <memory>
#include <numeric>

#include "core/log.h"
#include "core/text.h"
#include "doc/table.h"
#include "gfx/font.h"
#include "services/library.h"
#include "ui/app.h"
#include "ui/i18n.h"
#include "ui/overlays.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

// Text size, kept for the session (stepped in the menu).
int g_text_px = 26;

// A cell's text on one line: line breaks and tabs become spaces.
std::string one_line(std::string_view v) {
    if (v.find_first_of("\r\n\t") == std::string_view::npos) return std::string(v);
    return lib::collapse_space(v);
}

// Numeric value of a cell for sorting ("1,234.5", "$12", "45%").
bool number_of(std::string_view v, double& out) {
    if (!doc::looks_numeric(v)) return false;
    std::string s;
    bool negative = false;
    for (char ch : v) {
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E') s += ch;
        else if (ch == '-' || ch == '(') negative = true;
    }
    if (s.empty()) return false;
    out = strtod(s.c_str(), nullptr) * (negative ? -1 : 1);
    return true;
}

// ---- the row card ------------------------------------------------------------------------

class RowCard : public Screen {
public:
    RowCard(std::shared_ptr<doc::Table> table, std::vector<int> order, int index, std::function<void(int)> moved)
        : t_(std::move(table)), order_(std::move(order)), index_(index), moved_(std::move(moved)) {}

    bool full_screen() const override { return true; }
    std::vector<Hint> hints() const override {
        return {{kLeft, "Previous / next row"}, {kUp, "Scroll"}, {kCircle, "Back"}};
    }

    void update(const Input& in) override {
        if (in.press(kCircle) || in.press(kCross)) {
            app().pop();
            return;
        }
        if (in.repeat(kDown)) scroll_ += 120;
        if (in.repeat(kUp)) scroll_ = std::max(0, scroll_ - 120);
        if (in.repeat(kR1)) scroll_ += 700;
        if (in.repeat(kL1)) scroll_ = std::max(0, scroll_ - 700);
        const int count = (int)order_.size();
        if (in.repeat(kRight) && index_ + 1 < count) go(index_ + 1);
        if (in.repeat(kLeft) && index_ > 0) go(index_ - 1);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& th = theme();
        draw_background(c);
        const int row = order_[index_];
        const std::string title = T("Row") + std::string(" ") + std::to_string(index_ + 1) + " / " +
                                  std::to_string(order_.size());
        gfx::draw_text(c, Face::SansBold, kTitlePx, kMarginX, 48, title, th.text);
        const Rect area{kMarginX, 130, kContentW, kFooterTop - 150};
        c.push_clip(area);
        int y = area.y - scroll_;
        const int label_w = 520;
        for (int col = 0; col < t_->columns(); ++col) {
            std::string name = t_->header ? one_line(t_->cell(0, col)) : std::string();
            if (name.empty()) name = T("Column") + std::string(" ") + std::to_string(col + 1);
            const std::string value(t_->cell(row, col));
            const auto lines = gfx::wrap_text(Face::Sans, kBodyPx, value.empty() ? "\xE2\x80\x94" : value,
                                              area.w - label_w - 40, 40);
            const int h = std::max(1, (int)lines.size()) * (kBodyPx * 3 / 2) + 24;
            if (y + h > area.y && y < area.bottom()) {
                if (col % 2 == 0) c.fill_rounded({area.x, y, area.w, h}, 10, gfx::with_alpha(th.surface, 160));
                gfx::draw_text(c, Face::SansSemiBold, kBodyPx, area.x + 24, y + 12,
                               gfx::ellipsize(Face::SansSemiBold, kBodyPx, name, label_w - 48), th.muted);
                int ly = y + 12;
                for (const auto& line : lines) {
                    gfx::draw_text(c, Face::Sans, kBodyPx, area.x + label_w, ly, line,
                                   value.empty() ? th.faint : th.text);
                    ly += kBodyPx * 3 / 2;
                }
            }
            y += h + 6;
        }
        content_h_ = y + scroll_ - area.y;
        c.pop_clip();
        scroll_ = std::max(0, std::min(scroll_, std::max(0, content_h_ - area.h)));
        draw_footer(c, hints());
    }

private:
    void go(int index) {
        index_ = index;
        scroll_ = 0;
        if (moved_) moved_(index_);
    }

    std::shared_ptr<doc::Table> t_;
    std::vector<int> order_;
    int index_ = 0, scroll_ = 0, content_h_ = 0;
    std::function<void(int)> moved_;
};

// ---- the grid ----------------------------------------------------------------------------

enum class Sort { None, Up, Down };

class TableViewer : public Screen {
public:
    explicit TableViewer(svc::Item item) : item_(std::move(item)) {
        const std::string path = item_.path;
        lib::async<std::shared_ptr<doc::Table>>(
            token_,
            [path] {
                auto t = std::make_shared<doc::Table>();
                std::string error;
                if (!doc::open_table(path, *t, error)) {
                    LOG("table: %s not opened: %s", path.c_str(), error.c_str());
                    t.reset();
                    t = std::make_shared<doc::Table>();
                    t->delimiter = 0;  // marks the failure; the reason follows
                    t->add_cell(error);
                    t->finish();
                }
                return t;
            },
            [this](std::shared_ptr<doc::Table>& t) {
                if (t->delimiter == 0) {
                    error_ = std::string(t->cell(0, 0));
                    return;
                }
                table_ = t;
                rebuild_order();
                // Where the reader left off (row, column).
                cursor_row_ = std::max(0, std::min((int)order_.size() - 1, item_.position.section));
                cursor_col_ = std::max(0, std::min(table_->columns() - 1, item_.position.block));
                if (table_->truncated)
                    app().toast(T("Only the first rows of this large table are shown."));
            });
    }

    ~TableViewer() override { save(); }

    bool full_screen() const override { return true; }
    std::vector<Hint> hints() const override {
        if (!table_) return {{kCircle, "Back"}};
        return {{kCross, "Row details"}, {kSquare, "Find"}, {kTriangle, "Options"}, {kCircle, "Back"}};
    }

    void update(const Input& in) override {
        if (in.press(kCircle)) {
            save();
            app().pop();
            return;
        }
        if (!table_) return;
        const int rows = (int)order_.size(), cols = table_->columns();
        if (in.repeat(kDown)) cursor_row_ = std::min(rows - 1, cursor_row_ + 1);
        if (in.repeat(kUp)) cursor_row_ = std::max(0, cursor_row_ - 1);
        if (in.repeat(kRight)) cursor_col_ = std::min(cols - 1, cursor_col_ + 1);
        if (in.repeat(kLeft)) cursor_col_ = std::max(0, cursor_col_ - 1);
        if (in.repeat(kR1)) cursor_row_ = std::min(rows - 1, cursor_row_ + std::max(1, visible_rows_ - 1));
        if (in.repeat(kL1)) cursor_row_ = std::max(0, cursor_row_ - std::max(1, visible_rows_ - 1));
        if (in.repeat(kR2)) cursor_col_ = std::min(cols - 1, cursor_col_ + std::max(1, visible_cols_ - 1));
        if (in.repeat(kL2)) cursor_col_ = std::max(0, cursor_col_ - std::max(1, visible_cols_ - 1));
        if (in.press(kCross) && rows > 0) open_card();
        if (in.press(kSquare)) find_prompt();
        if (in.press(kTriangle) || in.press(kOptions)) menu();
    }

    void draw(gfx::Canvas& c) override {
        const Theme& th = theme();
        draw_background(c);
        if (!table_) {
            const Rect all{0, 0, kWidth, kFooterTop};
            if (!error_.empty()) draw_message(c, all, T("Could not open this table"), error_, true);
            else draw_loading(c, all, item_.title);
            draw_footer(c, hints());
            return;
        }
        measure_columns();
        const int px = g_text_px;
        const int row_h = px * 7 / 4;
        const int rows = (int)order_.size(), cols = table_->columns();

        // Title and where the cursor is.
        gfx::draw_text(c, Face::SansBold, 34, 60, 34, gfx::ellipsize(Face::SansBold, 34, item_.title, 1100), th.text);
        std::string where = T("Row") + std::string(" ") + group(cursor_row_ + 1) + " / " + group(rows) + "   \xC2\xB7   " +
                            column_name(cursor_col_);
        if (sort_ != Sort::None) where += "   \xC2\xB7   " + std::string(T(sort_ == Sort::Up ? "Ascending" : "Descending"));
        const int ww = gfx::text_width(Face::Sans, 24, where);
        gfx::draw_text(c, Face::Sans, 24, 1860 - ww, 42, where, th.muted);

        // The grid: frozen header row and row numbers.
        const Rect grid{60, 100, 1800, 720};
        const int gutter = std::max(70, gfx::text_width(Face::Sans, px - 4, group(rows)) + 36);
        const int header_h = table_->header ? row_h : 0;
        visible_rows_ = std::max(1, (grid.h - header_h) / row_h);
        // Keep the cursor in view.
        if (cursor_row_ < top_) top_ = cursor_row_;
        if (cursor_row_ >= top_ + visible_rows_) top_ = cursor_row_ - visible_rows_ + 1;
        if (cursor_col_ < left_) left_ = cursor_col_;
        while (left_ < cursor_col_) {
            int w = 0;
            for (int col = left_; col <= cursor_col_; ++col) w += widths_[col];
            if (w <= grid.w - gutter) break;
            ++left_;
        }
        c.fill_rounded(grid, 12, gfx::with_alpha(th.surface, 140));
        c.push_clip(grid);
        // Columns that fit from left_.
        std::vector<std::pair<int, int>> shown;  // column, x
        int x = grid.x + gutter;
        for (int col = left_; col < cols && x < grid.right(); ++col) {
            shown.push_back({col, x});
            x += widths_[col];
        }
        visible_cols_ = 0;
        for (auto& [col, cx] : shown)
            if (cx + widths_[col] <= grid.right()) ++visible_cols_;
        visible_cols_ = std::max(1, visible_cols_);
        const int text_dy = (row_h - gfx::font_metrics(Face::Sans, px).line_height) / 2;
        int y = grid.y + header_h;
        for (int i = top_; i < rows && i < top_ + visible_rows_; ++i, y += row_h) {
            const int row = order_[i];
            const bool current = i == cursor_row_;
            if (current) c.fill({grid.x, y, grid.w, row_h}, gfx::with_alpha(th.accent, 46));
            else if (i % 2) c.fill({grid.x, y, grid.w, row_h}, gfx::with_alpha(th.surface_hi, 70));
            const std::string num = group(i + 1);
            gfx::draw_text(c, Face::Sans, px - 4, grid.x + gutter - 18 - gfx::text_width(Face::Sans, px - 4, num),
                           y + text_dy + 2, num, current ? th.text : th.faint);
            for (auto& [col, cx] : shown) {
                const int w = widths_[col];
                const bool cell_focus = current && col == cursor_col_;
                if (cell_focus) c.fill_rounded({cx + 2, y + 2, w - 4, row_h - 4}, 6, th.accent);
                draw_cell(c, table_->cell(row, col), cx, y + text_dy, w, px, table_->numeric(col),
                          cell_focus ? th.accent_text : th.text, Face::Sans);
            }
        }
        // Header row and gutter edge, over the rows.
        if (table_->header) {
            c.fill({grid.x, grid.y, grid.w, header_h}, th.surface_hi);
            for (auto& [col, cx] : shown)
                draw_cell(c, table_->cell(0, col), cx, grid.y + text_dy, widths_[col], px, table_->numeric(col),
                          col == cursor_col_ ? th.accent : th.text, Face::SansSemiBold);
            c.fill({grid.x, grid.y + header_h - 2, grid.w, 2}, th.divider);
        }
        c.fill({grid.x + gutter - 8, grid.y, 2, grid.h}, th.divider);
        c.pop_clip();
        draw_scrollbar(c, {grid.right() + 10, grid.y + header_h, 6, grid.h - header_h}, top_, visible_rows_, rows);

        // The selected cell in full.
        const Rect detail{60, 838, 1800, 140};
        c.fill_rounded(detail, 12, th.surface);
        gfx::draw_text(c, Face::SansSemiBold, 22, detail.x + 24, detail.y + 14,
                       gfx::ellipsize(Face::SansSemiBold, 22, column_name(cursor_col_), detail.w - 48), th.muted);
        const std::string value(table_->cell(order_.empty() ? 0 : order_[cursor_row_], cursor_col_));
        gfx::draw_wrapped(c, Face::Sans, 26, detail.x + 24, detail.y + 48, detail.w - 48,
                          value.empty() ? std::string(T("(empty)")) : one_line(value), value.empty() ? th.faint : th.text, 2);
        draw_footer(c, hints());
    }

private:
    void draw_cell(gfx::Canvas& c, std::string_view v, int x, int y, int w, int px, bool right, gfx::Color color,
                   Face face) {
        if (v.empty()) return;
        const std::string text = gfx::ellipsize(face, px, one_line(v), w - 28);
        const int tx = right ? x + w - 14 - gfx::text_width(face, px, text) : x + 14;
        gfx::draw_text(c, face, px, tx, y, text, color);
    }

    // Column widths from the first rows' text, at the current size.
    void measure_columns() {
        if (measured_px_ == g_text_px && (int)widths_.size() == table_->columns()) return;
        measured_px_ = g_text_px;
        const int px = g_text_px;
        widths_.assign(table_->columns(), 0);
        const int sample = std::min(table_->rows(), 300);
        for (int col = 0; col < table_->columns(); ++col) {
            int w = 0;
            for (int r = 0; r < sample; ++r) {
                const std::string_view v = table_->cell(r, col);
                if (v.empty()) continue;
                const Face f = r == 0 && table_->header ? Face::SansSemiBold : Face::Sans;
                // Long text is capped: the detail strip and the row card show it whole.
                w = std::max(w, gfx::text_width(f, px, one_line(v.substr(0, lib::utf8_prefix(v, 60)))));
            }
            widths_[col] = std::max(px * 4, std::min(w + 28, px * 22));
        }
    }

    std::string column_name(int col) const {
        std::string name = table_ && table_->header ? one_line(table_->cell(0, col)) : std::string();
        const std::string number = T("Column") + std::string(" ") + std::to_string(col + 1);
        return name.empty() ? number : name + "  (" + number + ")";
    }

    static std::string group(int n) {
        std::string s = std::to_string(n);
        for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
        return s;
    }

    // The rows shown, in display order (without the header row).
    void rebuild_order() {
        const int keep = order_.empty() ? -1 : order_[std::min(cursor_row_, (int)order_.size() - 1)];
        const int first = table_->header ? 1 : 0;
        order_.resize(std::max(0, table_->rows() - first));
        std::iota(order_.begin(), order_.end(), first);
        if (sort_ != Sort::None) {
            const int col = sort_col_;
            const doc::Table& t = *table_;
            const bool numeric = t.numeric(col);
            std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) {
                const std::string_view x = t.cell(a, col), y = t.cell(b, col);
                // Empty cells last, whichever the direction.
                if (x.empty() != y.empty()) return y.empty();
                bool less;
                double dx, dy;
                if (numeric && number_of(x, dx) && number_of(y, dy)) {
                    if (dx == dy) return false;
                    less = dx < dy;
                } else {
                    if (lib::iequals(x, y)) return false;
                    less = lib::natural_less(lib::lower(x), lib::lower(y));
                }
                return sort_ == Sort::Up ? less : !less;
            });
        }
        if (keep >= 0) {
            auto it = std::find(order_.begin(), order_.end(), keep);
            cursor_row_ = it == order_.end() ? 0 : (int)(it - order_.begin());
        }
    }

    void open_card() {
        app().push(std::make_unique<RowCard>(table_, order_, cursor_row_, [this](int index) { cursor_row_ = index; }));
    }

    void find_prompt() {
        open_keyboard(T("Find in the table"), find_, KeyboardKind::Text, [this](const std::string& text) {
            find_ = lib::trim(text);
            find_next(false);
        });
    }

    // Next cell containing the search text, row by row from the cursor.
    void find_next(bool skip_current) {
        if (find_.empty() || !table_) return;
        const int rows = (int)order_.size(), cols = table_->columns();
        const long total = (long)rows * cols;
        long at = (long)cursor_row_ * cols + cursor_col_ + (skip_current ? 1 : 0);
        for (long n = 0; n < total; ++n, ++at) {
            const long i = at % total;
            const int r = (int)(i / cols), col = (int)(i % cols);
            if (lib::icontains(table_->cell(order_[r], col), find_)) {
                if (i < (long)cursor_row_ * cols + cursor_col_ && n > 0) app().toast(T("Search continued from the top."));
                cursor_row_ = r;
                cursor_col_ = col;
                return;
            }
        }
        app().toast(T("No cell contains") + std::string(" “") + find_ + "”.");
    }

    void go_to_row() {
        open_keyboard(T("Go to row"), "", KeyboardKind::Text, [this](const std::string& text) {
            const int n = atoi(lib::trim(text).c_str());
            if (n >= 1) cursor_row_ = std::min((int)order_.size() - 1, n - 1);
        });
    }

    void menu() {
        if (!table_) return;
        std::vector<MenuItem> items;
        items.push_back({T("Find"), [this] { return find_; }, [this] { find_prompt(); }, nullptr});
        if (!find_.empty()) items.push_back({T("Find next"), nullptr, [this] { find_next(true); }, nullptr});
        items.push_back({T("Go to row"), nullptr, [this] { go_to_row(); }, nullptr});
        items.push_back({T("First row"), nullptr, [this] { cursor_row_ = 0; }, nullptr});
        items.push_back({T("Last row"), nullptr, [this] { cursor_row_ = std::max(0, (int)order_.size() - 1); }, nullptr});
        const std::string col = column_name(cursor_col_);
        items.push_back({T("Sort by this column"),
                         [this] {
                             if (sort_ == Sort::None || sort_col_ != cursor_col_) return std::string(T("Original order"));
                             return std::string(T(sort_ == Sort::Up ? "Ascending" : "Descending"));
                         },
                         [this] { step_sort(1); },
                         [this](int d) { step_sort(d); }, true});
        items.push_back({T("First row is a header"), [this] { return std::string(T(table_->header ? "On" : "Off")); },
                         [this] {
                             table_->header = !table_->header;
                             measured_px_ = 0;
                             rebuild_order();
                         },
                         nullptr, true});
        items.push_back({T("Text size"), [] { return std::to_string(g_text_px) + " px"; }, nullptr,
                         [](int d) { g_text_px = std::max(18, std::min(40, g_text_px + 2 * d)); }, true});
        open_menu(item_.title, std::move(items), col);
    }

    void step_sort(int d) {
        if (sort_col_ != cursor_col_) {
            sort_col_ = cursor_col_;
            sort_ = Sort::None;
        }
        const int s = ((int)sort_ + (d > 0 ? 1 : 2)) % 3;
        sort_ = (Sort)s;
        rebuild_order();
    }

    void save() {
        if (!table_ || order_.empty()) return;
        // The row as in the file, so a sort does not move the bookmark.
        const int first = table_->header ? 1 : 0;
        svc::Position p;
        p.section = sort_ == Sort::None ? cursor_row_ : order_[cursor_row_] - first;
        p.block = cursor_col_;
        const int n = (int)order_.size();
        svc::library().set_position(item_.id, p, n > 1 ? (float)p.section / (n - 1) : 1.f);
    }

    svc::Item item_;
    std::shared_ptr<doc::Table> table_;
    std::string error_, find_;
    std::vector<int> order_, widths_;
    int cursor_row_ = 0, cursor_col_ = 0, top_ = 0, left_ = 0;
    int visible_rows_ = 10, visible_cols_ = 4, measured_px_ = 0;
    Sort sort_ = Sort::None;
    int sort_col_ = 0;
};

}  // namespace

void open_table_viewer(const svc::Item& item) { app().push(std::make_unique<TableViewer>(item)); }

}  // namespace ui
