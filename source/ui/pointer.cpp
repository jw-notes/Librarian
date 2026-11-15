// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/pointer.h"
#include <algorithm>
#include <cmath>
#include "platform/platform.h"

namespace ui {
bool Pointer::update(int rx, int ry, uint64_t now, int speed) {
    const bool was = shown_;
    const float dt = last_tick_ ? std::min<uint64_t>(100, now - last_tick_) / 1000.f : 1.f / 60;
    last_tick_ = now;
    const float magnitude = std::sqrt((float)rx * rx + (float)ry * ry);
    if (magnitude > 0) {
        const float strength = std::min(1.f, magnitude / 127.f);
        const float distance = 900.f * std::clamp(speed, 25, 250) / 100 * strength * strength * dt;
        x_ = std::clamp(x_ + rx / magnitude * distance, 0.f, (float)platform::kWidth - 1);
        y_ = std::clamp(y_ + ry / magnitude * distance, 0.f, (float)platform::kHeight - 1);
        moved_at_ = now;
        shown_ = true;
        return true;
    }
    if (shown_ && now - moved_at_ >= 3500) shown_ = false;
    return was != shown_;
}
void Pointer::draw(gfx::Canvas& c, bool clickable) const {
    if (!shown_) return;
    const auto color = gfx::rgb(clickable ? 0x70C6FF : 0xFFFFFF);
    c.line(x_, y_, x_ + 7, y_ + 24, 7, gfx::rgb(0x10141B));
    c.line(x_, y_, x_ + 20, y_ + 15, 7, gfx::rgb(0x10141B));
    c.line(x_ + 7, y_ + 24, x_ + 20, y_ + 15, 7, gfx::rgb(0x10141B));
    c.line(x_, y_, x_ + 7, y_ + 24, 3, color);
    c.line(x_, y_, x_ + 20, y_ + 15, 3, color);
    c.line(x_ + 7, y_ + 24, x_ + 20, y_ + 15, 3, color);
}
int text_link_at(const doc::Layout& layout, int x, int y, int column_x, int top, int view, int height) {
    if (y < top || y >= top + height) return -1;
    return layout.link_at(x - column_x, y - top + view);
}
}  // namespace ui
