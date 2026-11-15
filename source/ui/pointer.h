// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <stdint.h>
#include "gfx/canvas.h"
#include "doc/layout.h"

namespace ui {
// Coordinates are screen pixels; callers translate through the current viewport.
class Pointer {
public:
    bool update(int rx, int ry, uint64_t now, int speed);
    void hide() { shown_ = false; last_tick_ = 0; }
    bool shown() const { return shown_; }
    int x() const { return (int)x_; }
    int y() const { return (int)y_; }
    void draw(gfx::Canvas& c, bool clickable) const;
private:
    float x_ = 960, y_ = 540;
    uint64_t last_tick_ = 0, moved_at_ = 0;
    bool shown_ = false;
};
// Translate a screen point to the currently drawn document column.
int text_link_at(const doc::Layout& layout, int screen_x, int screen_y,
                 int column_x, int content_top, int view_top, int visible_height);
}  // namespace ui
