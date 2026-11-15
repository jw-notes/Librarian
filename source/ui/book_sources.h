// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include "services/library.h"

namespace ui {

// Opens a local book in the viewer for its format (reader for EPUB, text and
// HTML; page viewer for comics and PDF).
void open_book_file(const svc::Item& item);

}  // namespace ui
