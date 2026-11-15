// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <memory>
#include <string>

#include "services/kiwix.h"
#include "ui/screen.h"

namespace ui {

std::unique_ptr<Screen> make_knowledge_screen();
void open_kiwix_collection(const svc::ZimBook& book);
// Opens the search screen and its keyboard.
void open_kiwix_search(const std::string& content, const std::string& title);
// The Kiwix online library (ui/kiwix_online.cpp): pushed, or as the Knowledge
// tab itself when no Kiwix Server is set up.
void open_kiwix_online_library();
// A ZIM file from anywhere (the Local section): its collection page.
void open_zim_file(const std::string& path);
std::unique_ptr<Screen> make_kiwix_online_root();
// Shared with the online library's screens.
std::string kiwix_language_name(const std::string& code);
void draw_kiwix_icon(gfx::Canvas& c, const gfx::Rect& r, const svc::ZimBook& book);

}  // namespace ui
