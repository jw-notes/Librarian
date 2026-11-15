// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Opening things: library items, files and Kiwix articles, each in the right
// viewer for its format.
#include <string>

#include "services/library.h"

namespace ui {

void open_item(const svc::Item& item);
// Opens a local document; creates its library entry if needed.
void open_file(const std::string& path, const std::string& title = "", const std::string& author = "",
               const std::string& cover = "", const std::string& source = "local", const std::string& remote_id = "");
void open_article(const std::string& url, const std::string& collection_title = "");
bool is_supported_file(const std::string& path);

}  // namespace ui
