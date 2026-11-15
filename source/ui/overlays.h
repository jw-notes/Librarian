// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Overlays drawn over the current screen: the on-screen keyboard and a side
// menu used for options, reader settings and confirmations.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/screen.h"
#include "ui/widgets.h"

namespace ui {

enum class KeyboardKind { Text, Url, Password };

// Opens the keyboard; `done` receives the text when the user confirms.
void open_keyboard(const std::string& title, const std::string& initial, KeyboardKind kind,
                   std::function<void(const std::string&)> done);

struct MenuItem {
    std::string label;
    // Current value shown on the right ("34 px", "On"); may be empty.
    std::function<std::string()> value;
    // X on the item. The menu closes afterwards unless `stay_open`.
    std::function<void()> activate;
    // Left / Right on the item (values that step).
    std::function<void(int)> adjust;
    bool stay_open = false;
    bool destructive = false;
};

void open_menu(const std::string& title, std::vector<MenuItem> items, const std::string& subtitle = "");
// A yes/no question; `yes` runs when confirmed.
void confirm(const std::string& title, const std::string& message, const std::string& yes_label,
             std::function<void()> yes);

}  // namespace ui
