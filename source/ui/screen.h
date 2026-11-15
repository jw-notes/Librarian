// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <string>
#include <vector>

#include "core/worker.h"
#include "gfx/canvas.h"
#include "ui/input.h"

namespace ui {

// One button prompt in the footer: "✕ Open".
struct Hint {
    uint32_t button;
    std::string label;
};

// Audiobooks is shown only once an Audiobookshelf server is set up or a
// downloaded audiobook is found (see tab_visible).
enum class Tab { None = -1, Home = 0, Knowledge, Books, Audiobooks, Local, Settings, Count };

class Screen {
public:
    virtual ~Screen() { *token_ = true; }
    virtual void update(const Input& in) = 0;
    virtual void draw(gfx::Canvas& c) = 0;
    // True while something moves on its own (a spinner, smooth scrolling).
    virtual bool animating() const { return false; }
    // Called when the screen becomes the top one again.
    virtual void resumed() {}
    // Overlays (menus, the keyboard, dialogs) draw over the screen below.
    virtual bool overlay() const { return false; }
    // Full-screen screens (the reader) have no header or footer.
    virtual bool full_screen() const { return false; }
    virtual Tab tab() const { return Tab::None; }
    virtual std::vector<Hint> hints() const { return {}; }

protected:
    // Cancelled when the screen is destroyed: results of background work for a
    // closed screen are dropped.
    lib::CancelToken token_ = lib::make_token();
};

}  // namespace ui
