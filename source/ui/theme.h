// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include "doc/layout.h"
#include "gfx/canvas.h"
#include "services/settings.h"

namespace ui {

struct Theme {
    gfx::Color bg, bg_top, surface, surface_hi, text, muted, faint, accent, accent_text, divider, focus, error, success;
};

struct ReaderColors {
    gfx::Color page, text, heading, muted, link, link_focus_bg, code, code_bg, rule, quote_bar, table_border, table_header,
        image_placeholder;
};

const Theme& theme();
ReaderColors reader_colors(svc::ReaderTheme t);
gfx::Color role_color(const ReaderColors& c, doc::Role role);

}  // namespace ui
