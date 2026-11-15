// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/audiobookshelf.h"
#include "services/listening.h"
#include "services/local_library.h"
#include "services/settings.h"
#include "services/zim_shelf.h"
#include "ui/app.h"
#include "ui/i18n.h"
#include "ui/kiwix_screens.h"

namespace ui {

std::unique_ptr<Screen> make_home_screen();
std::unique_ptr<Screen> make_settings_screen();
std::unique_ptr<Screen> make_books_screen();
std::unique_ptr<Screen> make_audiobooks_screen();
std::unique_ptr<Screen> make_local_screen();

std::unique_ptr<Screen> make_tab_screen(Tab tab) {
    switch (tab) {
        case Tab::Knowledge: return make_knowledge_screen();
        case Tab::Books: return make_books_screen();
        case Tab::Audiobooks: return make_audiobooks_screen();
        case Tab::Local: return make_local_screen();
        case Tab::Settings: return make_settings_screen();
        default: return make_home_screen();
    }
}

// Without any server only Home, Local and Settings are shown; each server's
// tab appears once its address is set (Knowledge also for the Kiwix online
// library, Books also for OPDS catalogues such as Project Gutenberg).
bool tab_visible(Tab tab) {
    const svc::Settings& s = svc::settings();
    switch (tab) {
        case Tab::Knowledge: return !s.kiwix_url.empty() || s.kiwix_online || svc::zim_shelf().count() > 0;
        case Tab::Books: return !s.calibre_url.empty() || !s.opds_catalogs.empty();
        case Tab::Audiobooks:
            return svc::abs::configured() || s.librivox || !svc::listening().downloads().empty() ||
                   svc::listening().active() || !svc::local_library().talking_books().empty();
        case Tab::Home:
        case Tab::Local:
        case Tab::Settings: return true;
        default: return false;
    }
}

const char* tab_name(Tab tab) {
    static const char* kNames[] = {"Home", "Knowledge", "Books", "Audiobooks", "Local", "Settings"};
    return tab >= Tab::Home && tab < Tab::Count ? T(kNames[(int)tab]) : "";
}

}  // namespace ui
