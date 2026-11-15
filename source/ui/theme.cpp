// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/theme.h"

namespace ui {

using gfx::rgb;

const Theme& theme() {
    // Dark is tuned for a television in a dim room: no pure white, no pure black.
    static const Theme dark{rgb(0x12151C), rgb(0x1A1F2B), rgb(0x1E2431), rgb(0x2A3245), rgb(0xE8ECF3), rgb(0x9BA5B8),
                            rgb(0x5C6577), rgb(0x4C8DFF), rgb(0xFFFFFF), rgb(0x2C3446), rgb(0x6FA4FF), rgb(0xFF6B6B),
                            rgb(0x4CC38A)};
    static const Theme light{rgb(0xF3F4F7), rgb(0xE6E9F0), rgb(0xFFFFFF), rgb(0xE3E9F6), rgb(0x1B2030), rgb(0x5A6275),
                             rgb(0x9AA1B0), rgb(0x2463EB), rgb(0xFFFFFF), rgb(0xD5D9E2), rgb(0x2463EB), rgb(0xD93838),
                             rgb(0x16915A)};
    return svc::settings().ui_theme == 1 ? light : dark;
}

ReaderColors reader_colors(svc::ReaderTheme t) {
    switch (t) {
        case svc::ReaderTheme::Light:
            return {rgb(0xFAFAF8), rgb(0x1E1E1E), rgb(0x111111), rgb(0x6B6B6B), rgb(0x1F5FC4), rgb(0xD6E4FF),
                    rgb(0x333333), rgb(0xEFEFEC), rgb(0xD0D0CC), rgb(0xC8C8C2), rgb(0xD8D8D3), rgb(0xEDEDE8),
                    rgb(0xE6E6E2)};
        case svc::ReaderTheme::Sepia:
            return {rgb(0xF4ECD8), rgb(0x433422), rgb(0x2F2416), rgb(0x7D6A52), rgb(0x8A4B12), rgb(0xE6D3AA),
                    rgb(0x433422), rgb(0xEADFC6), rgb(0xD6C7A6), rgb(0xC7B48D), rgb(0xD9CBAA), rgb(0xEADCBD),
                    rgb(0xE8DDC4)};
        case svc::ReaderTheme::Black:
            return {rgb(0x000000), rgb(0xC9CDD4), rgb(0xE6E8EC), rgb(0x7D838E), rgb(0x7FB0FF), rgb(0x1C2A44),
                    rgb(0xC9CDD4), rgb(0x15171B), rgb(0x2A2E35), rgb(0x3A404A), rgb(0x2C3038), rgb(0x16191E),
                    rgb(0x111316)};
        case svc::ReaderTheme::Dark:
        default:
            return {rgb(0x16191F), rgb(0xD8DCE3), rgb(0xF0F2F5), rgb(0x8C94A3), rgb(0x7FB0FF), rgb(0x24365A),
                    rgb(0xD8DCE3), rgb(0x20242C), rgb(0x343A46), rgb(0x4A5262), rgb(0x343A46), rgb(0x232832),
                    rgb(0x1E222A)};
    }
}

gfx::Color role_color(const ReaderColors& c, doc::Role role) {
    switch (role) {
        case doc::Role::Body: return c.text;
        case doc::Role::Heading: return c.heading;
        case doc::Role::Muted: return c.muted;
        case doc::Role::Link: return c.link;
        case doc::Role::Code: return c.code;
        case doc::Role::Rule: return c.rule;
        case doc::Role::CodeBackground: return c.code_bg;
        case doc::Role::QuoteBar: return c.quote_bar;
        case doc::Role::TableBorder: return c.table_border;
        case doc::Role::TableHeader: return c.table_header;
        case doc::Role::Marker: return c.muted;
    }
    return c.text;
}

}  // namespace ui
