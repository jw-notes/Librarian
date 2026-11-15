// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "ui/screen.h"

namespace ui {

class App {
public:
    void run();
    void push(std::unique_ptr<Screen> screen);
    // Closes the top screen (at the next frame, so a screen may close itself).
    void pop();
    // Replaces the whole stack with a tab's root screen.
    void switch_tab(Tab tab);
    void toast(const std::string& message);
    void quit() { quit_ = true; }
    // Closes every screen (at exit, while the services they use still run).
    void close_all() {
        pending_push_.clear();
        pending_root_.reset();
        while (!stack_.empty()) stack_.pop_back();
    }
    // The screen below the top one (for overlays that act on it).
    Screen* below(const Screen* s);

private:
    void apply_pending();
    void draw();

    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> pending_push_;
    int pending_pops_ = 0;
    std::unique_ptr<Screen> pending_root_;
    std::string toast_;
    uint64_t toast_until_ = 0, last_tick_ = 0;
    bool quit_ = false;
    InputReader input_;
};

App& app();
// Each tab's root screen (defined in the screens' files).
std::unique_ptr<Screen> make_tab_screen(Tab tab);
bool tab_visible(Tab tab);
const char* tab_name(Tab tab);
// Adding an OPDS catalogue (Project Gutenberg, or one typed in). The new one
// becomes the catalogue the Books section shows; `added` runs afterwards.
void open_add_catalogue_menu(std::function<void()> added = nullptr);
// The Books section's catalogues, to pick which one it shows or add another.
void open_choose_catalogue_menu(std::function<void()> changed = nullptr);
// The audiobook player, and the strip shown in the footer while one plays.
void open_player();
void draw_now_playing(gfx::Canvas& c);

}  // namespace ui
