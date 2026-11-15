// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The interface language: English, 日本語, Português (Brasil), Deutsch,
// Français, Español. Every string is written in English where it is used and
// looked up by that text (i18n_catalog.cpp); one with no entry stays English.
// The shared drawing points translate what they are given (button hints,
// messages, menus, dialogs, toasts, tab names, Settings rows), so most screens
// need no changes. Only Librarian's own words are translated: book titles,
// articles and server answers are shown as they are.
#include <string>

namespace ui {

enum class Lang { En, Ja, PtBr, De, Fr, Es, Count };

// The text in the current language (`en` itself when there is none).
const char* T(const char* en);
std::string T(const std::string& en);

// Settings › Appearance › Language: "auto" follows the console, or a code
// ("ja", "pt-BR", "de", "fr", "es", "en").
void language_apply();
void language_step(int dir);
std::string language_label();
Lang current_language();

// For tests: every catalog entry, and whether each has all its languages
// with the same printf conversions as the English.
struct CatalogCheck {
    size_t entries = 0;
    std::string problem;  // empty when all is well
};
CatalogCheck check_catalog();

}  // namespace ui
