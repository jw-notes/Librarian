// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/overlays.h"
#include "ui/i18n.h"

#include <algorithm>

#include "core/text.h"
#include "gfx/font.h"
#include "ui/app.h"

#ifdef LIB_HOST
#include "platform/host.h"
#endif

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

void dim(gfx::Canvas& c, int alpha = 170) { c.fill({0, 0, c.width(), c.height()}, gfx::with_alpha(gfx::rgb(0x05070B), alpha)); }

// ---- keyboard -----------------------------------------------------------------
// Controller buttons for the keyboard:
// D-pad moves (wrapping), X presses the key, Square deletes, Triangle is Caps
// (it stays on until pressed again), R2 switches letters / symbols, L1 / R1
// move the text cursor, OPTIONS is Done and Circle cancels.

class KeyboardScreen : public Screen {
public:
    KeyboardScreen(std::string title, std::string text, KeyboardKind kind, std::function<void(const std::string&)> done)
        : title_(std::move(title)), text_(std::move(text)), kind_(kind), done_(std::move(done)) {
        cursor_ = text_.size();
    }

    bool overlay() const override { return true; }

    std::vector<Hint> hints() const override {
        return {{kCircle, "Cancel"}, {kOptions, "Done"}, {kR2, "Letters / symbols"}, {kL1, "Cursor"},
                {kTriangle, "Caps"},  {kSquare, "Delete"}, {kCross, "Select"}};
    }

    void update(const Input& in) override {
#ifdef LIB_HOST
        std::string typed;
        if (host::take_typed_text(typed)) {
            text_ = typed;
            finish();
            return;
        }
#endif
        if (in.press(kCircle)) {
            app().pop();
            return;
        }
        if (in.press(kOptions)) {
            finish();
            return;
        }
        const auto& rows = layout();
        if (in.repeat(kUp)) row_ = (row_ + (int)rows.size()) % ((int)rows.size() + 1);
        if (in.repeat(kDown)) row_ = (row_ + 1) % ((int)rows.size() + 1);
        if (row_ < (int)rows.size()) {
            const int n = (int)rows[row_].size();
            if (in.repeat(kLeft)) col_ = (col_ + n - 1) % n;
            if (in.repeat(kRight)) col_ = (col_ + 1) % n;
            col_ = std::min(col_, n - 1);
        } else {
            if (in.repeat(kLeft)) action_ = (action_ + 4) % 5;
            if (in.repeat(kRight)) action_ = (action_ + 1) % 5;
        }
        if (in.repeat(kL1) && cursor_ > 0) {
            do --cursor_;
            while (cursor_ > 0 && ((unsigned char)text_[cursor_] & 0xC0) == 0x80);
        }
        if (in.repeat(kR1) && cursor_ < text_.size()) {
            size_t i = cursor_;
            lib::utf8_next(text_, i);
            cursor_ = i;
        }
        if (in.repeat(kSquare)) backspace();
        if (in.press(kTriangle)) toggle_caps();
        if (in.press(kR2)) symbols_ = !symbols_;
        if (in.press(kCross)) {
            if (row_ < (int)rows.size()) {
                std::string key = rows[row_][col_];
                if (shift_ && key.size() == 1) key[0] = (char)toupper((unsigned char)key[0]);
                insert(key);
                if (shift_ && !caps_) shift_ = false;
            } else {
                switch (action_) {
                    case 0: shift_ = !shift_; caps_ = false; break;
                    case 1: symbols_ = !symbols_; break;
                    case 2: insert(" "); break;
                    case 3: backspace(); break;
                    case 4: finish(); break;
                }
            }
        }
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        dim(c);
        // Sized to the layout: the URL keyboard has an extra row.
        const int rows_h = (int)layout().size() * 84 + 84;
        const int h = 200 + rows_h + 30;
        const int w = 1240, x = (c.width() - w) / 2, y = std::max(140, (c.height() - h) / 2 - 20);
        c.fill_rounded({x, y, w, h}, 22, t.surface);
        gfx::draw_text(c, Face::SansSemiBold, kHeadPx, x + 48, y + 30, T(title_), t.text);
        // The field.
        const Rect field{x + 48, y + 92, w - 96, 72};
        c.fill_rounded(field, 12, t.bg);
        c.stroke_rounded(field, 12, 2, t.accent);
        std::string shown = text_;
        if (kind_ == KeyboardKind::Password) {
            shown.clear();
            for (size_t i = 0; i < text_.size();) {
                lib::utf8_next(text_, i);
                shown += "\xE2\x80\xA2";
            }
        }
        const size_t shown_cursor = kind_ == KeyboardKind::Password ? lib::utf8_prefix(shown, count_cp(text_.substr(0, cursor_))) : cursor_;
        const int px = 32;
        int before = gfx::text_width(Face::Sans, px, shown.substr(0, shown_cursor));
        const int avail = field.w - 40;
        const int shift_x = std::max(0, before - avail);
        c.push_clip({field.x + 16, field.y, field.w - 32, field.h});
        gfx::draw_text(c, Face::Sans, px, field.x + 20 - shift_x, field.y + 14, shown, t.text);
        c.fill({field.x + 20 - shift_x + before, field.y + 14, 3, 44}, t.accent);
        c.pop_clip();

        const auto& rows = layout();
        const int key = 100, gap = 12;
        int ky = y + 200;
        for (int r = 0; r < (int)rows.size(); ++r) {
            const int n = (int)rows[r].size();
            int kx = x + (w - (n * key + (n - 1) * gap)) / 2;
            for (int i = 0; i < n; ++i) {
                const bool on = r == row_ && i == col_;
                const Rect k{kx, ky, key, 72};
                c.fill_rounded(k, 10, on ? t.accent : t.surface_hi);
                std::string label = rows[r][i];
                if (shift_ && label.size() == 1) label[0] = (char)toupper((unsigned char)label[0]);
                const int kpx = label.size() > 2 ? 22 : 30;
                const int lw = gfx::text_width(Face::SansSemiBold, kpx, label);
                gfx::draw_text(c, Face::SansSemiBold, kpx, kx + (key - lw) / 2,
                               ky + (72 - gfx::font_metrics(Face::SansSemiBold, kpx).line_height) / 2, label,
                               on ? t.accent_text : t.text);
                kx += key + gap;
            }
            ky += 72 + gap;
        }
        // Actions row.
        static const char* kActions[] = {"Shift", "Symbols", "Space", "Delete", "Done"};
        static const int kWidths[] = {170, 190, 380, 170, 190};
        int total = 0;
        for (int wv : kWidths) total += wv + gap;
        int ax = x + (w - total + gap) / 2;
        for (int i = 0; i < 5; ++i) {
            const bool on = row_ == (int)rows.size() && action_ == i;
            const bool lit = (i == 0 && shift_) || (i == 1 && symbols_);
            const Rect k{ax, ky, kWidths[i], 72};
            c.fill_rounded(k, 10, on ? t.accent : lit ? gfx::mix(t.surface_hi, t.accent, 90) : t.surface_hi);
            const int lw = gfx::text_width(Face::SansSemiBold, 26, kActions[i]);
            gfx::draw_text(c, Face::SansSemiBold, 26, ax + (kWidths[i] - lw) / 2, ky + 18, kActions[i],
                           on ? t.accent_text : t.text);
            ax += kWidths[i] + gap;
        }
    }

private:
    static size_t count_cp(const std::string& s) {
        size_t n = 0;
        for (size_t i = 0; i < s.size();) {
            lib::utf8_next(s, i);
            ++n;
        }
        return n;
    }

    const std::vector<std::vector<std::string>>& layout() const {
        static const std::vector<std::vector<std::string>> text = {
            {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
            {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
            {"a", "s", "d", "f", "g", "h", "j", "k", "l", "'"},
            {"z", "x", "c", "v", "b", "n", "m", ",", ".", "-"}};
        static const std::vector<std::vector<std::string>> url = {
            {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
            {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
            {"a", "s", "d", "f", "g", "h", "j", "k", "l", ":"},
            {"z", "x", "c", "v", "b", "n", "m", ".", "/", "-"},
            {"http://", "https://", ".local", ":8080", ":8083", "_"}};
        static const std::vector<std::vector<std::string>> symbols = {
            {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"},
            {"/", ":", ";", "_", "=", "+", "?", "~", "\\", "|"},
            {"[", "]", "{", "}", "<", ">", "\"", "`", "\xE2\x82\xAC", "\xC2\xA3"},
            {"\xC3\xA9", "\xC3\xA8", "\xC3\xA0", "\xC3\xBC", "\xC3\xB6", "\xC3\xA4", "\xC3\xB1", "\xC3\xA7", "\xC3\x9F", "\xC3\xB8"}};
        if (symbols_) return symbols;
        return kind_ == KeyboardKind::Url ? url : text;
    }

    void insert(const std::string& s) {
        text_.insert(cursor_, s);
        cursor_ += s.size();
    }

    // Triangle: capitals until Triangle is pressed again.
    void toggle_caps() {
        caps_ = !caps_;
        shift_ = caps_;
    }

    void backspace() {
        if (cursor_ == 0) return;
        size_t start = cursor_ - 1;
        while (start > 0 && ((unsigned char)text_[start] & 0xC0) == 0x80) --start;
        text_.erase(start, cursor_ - start);
        cursor_ = start;
    }

    void finish() {
        auto done = done_;
        const std::string text = text_;
        app().pop();
        if (done) done(text);
    }

    std::string title_, text_;
    KeyboardKind kind_;
    std::function<void(const std::string&)> done_;
    size_t cursor_ = 0;
    int row_ = 1, col_ = 0, action_ = 4;
    bool shift_ = false, caps_ = false, symbols_ = false;
};

// ---- side menu ------------------------------------------------------------------

class MenuScreen : public Screen {
public:
    MenuScreen(std::string title, std::vector<MenuItem> items, std::string subtitle)
        : title_(std::move(title)), subtitle_(std::move(subtitle)), items_(std::move(items)) {}

    bool overlay() const override { return true; }

    std::vector<Hint> hints() const override {
        std::vector<Hint> h = {{kCircle, "Close"}};
        if (!items_.empty() && items_[nav_.index].adjust) h.insert(h.begin(), {kLeft | kRight, "Change"});
        else h.insert(h.begin(), {kCross, "Select"});
        return h;
    }

    void update(const Input& in) override {
        nav_.update(in, (int)items_.size(), visible(), false);
        if (in.press(kCircle) || in.press(kTriangle)) {
            app().pop();
            return;
        }
        if (items_.empty()) return;
        MenuItem& item = items_[nav_.index];
        if (item.adjust) {
            if (in.repeat(kLeft)) item.adjust(-1);
            if (in.repeat(kRight)) item.adjust(1);
        }
        if (in.press(kCross)) {
            if (item.activate) {
                auto action = item.activate;
                if (!item.stay_open) app().pop();
                action();
            } else if (item.adjust) {
                item.adjust(1);
            }
        }
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        dim(c, 140);
        const int w = 760, x = c.width() - w;
        c.fill({x, 0, w, c.height()}, t.surface);
        int y = 90;
        gfx::draw_text(c, Face::SansBold, 40, x + 56, y, gfx::ellipsize(Face::SansBold, 40, T(title_), w - 112), t.text);
        y += 64;
        if (!subtitle_.empty()) {
            y += gfx::draw_wrapped(c, Face::Sans, kSmallPx, x + 56, y, w - 112, T(subtitle_), t.muted, 3) + 10;
        }
        y += 20;
        const int row_h = 84;
        const int count = (int)items_.size();
        for (int i = nav_.top; i < std::min(count, nav_.top + visible()); ++i) {
            const MenuItem& item = items_[i];
            const bool on = i == nav_.index;
            const Rect r{x + 32, y, w - 64, row_h - 10};
            if (on) c.fill_rounded(r, 12, t.accent);
            const gfx::Color fg = on ? t.accent_text : item.destructive ? t.error : t.text;
            const int ty = r.y + (r.h - gfx::font_metrics(Face::Sans, kBodyPx).line_height) / 2;
            gfx::draw_text(c, Face::Sans, kBodyPx, r.x + 28, ty, gfx::ellipsize(Face::Sans, kBodyPx, T(item.label), r.w - 300), fg);
            if (item.value) {
                std::string v = T(item.value());
                if (item.adjust) v = "\xE2\x80\xB9  " + v + "  \xE2\x80\xBA";
                const int vw = gfx::text_width(Face::SansSemiBold, kBodyPx, v);
                gfx::draw_text(c, Face::SansSemiBold, kBodyPx, r.right() - 28 - vw, ty, v, on ? t.accent_text : t.muted);
            }
            y += row_h;
        }
        draw_scrollbar(c, {c.width() - 20, 240, 8, 720}, nav_.top, visible(), count);
    }

private:
    int visible() const { return subtitle_.empty() ? 9 : 8; }

    std::string title_, subtitle_;
    std::vector<MenuItem> items_;
    ListNav nav_;
};

}  // namespace

void open_keyboard(const std::string& title, const std::string& initial, KeyboardKind kind,
                   std::function<void(const std::string&)> done) {
    app().push(std::make_unique<KeyboardScreen>(title, initial, kind, std::move(done)));
}

void open_menu(const std::string& title, std::vector<MenuItem> items, const std::string& subtitle) {
    app().push(std::make_unique<MenuScreen>(title, std::move(items), subtitle));
}

void confirm(const std::string& title, const std::string& message, const std::string& yes_label,
             std::function<void()> yes) {
    std::vector<MenuItem> items;
    items.push_back({"Cancel", nullptr, [] {}, nullptr});
    MenuItem ok{yes_label, nullptr, std::move(yes), nullptr};
    ok.destructive = true;
    items.push_back(std::move(ok));
    open_menu(title, std::move(items), message);
}

}  // namespace ui
