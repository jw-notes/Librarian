// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// User settings, saved as settings.json in the data folder.
#include <string>
#include <vector>

namespace svc {

enum class ReaderTheme { Dark = 0, Light = 1, Sepia = 2, Black = 3 };

struct Settings {
    std::string kiwix_url;
    // The Kiwix online library (library.kiwix.org) in the Knowledge section,
    // which then shows even without a Kiwix Server.
    bool kiwix_online = false;
    // The download helper beside the Kiwix Server (extras/kiwix-helper): it
    // fetches collections from the online library into the server's folder.
    std::string kiwix_helper_url, kiwix_helper_key;
    std::string calibre_url, calibre_user, calibre_password;
    // Anonymous browsing: no credentials are sent, even if some are stored.
    bool calibre_anonymous = true;
    // Audiobookshelf: an account (user name and password), or an API key made
    // in its web interface (Settings › API Keys), which then takes precedence.
    std::string abs_url, abs_user, abs_password, abs_api_key;
    std::string abs_device_id;  // made once: the server lists sessions by device
    std::string abs_library;    // the library last browsed
    int listen_speed = 100;     // percent, 50..300
    bool allow_insecure_tls = false;
    // Where downloaded books are saved: false = console storage, true = a
    // connected USB drive (falls back to the console when none is connected).
    bool download_to_usb = false;

    int ui_theme = 0;  // 0 dark, 1 light
    std::string language = "auto";
    // The resolution drawn at: 0 = the TV's own, or 720 / 1080 / 2160.
    // Takes effect when Librarian starts.
    int resolution = 0;
    // Settings › System › Debug logging (core/log.h): detail lines in the log.
    bool debug_log = false;
    // Covers missing from local books, LibriVox books and audiobook folders are
    // looked up on Open Library by title and author (doc/covers.cpp).
    bool online_covers = true;  // "auto" (the console's) or a code: ui/i18n.h
    // Reading
    int font_px = 34;
    int line_spacing = 150;
    bool serif = true;
    bool justify = true;
    int margins = 1;  // 0 narrow, 1 normal, 2 wide
    ReaderTheme reader_theme = ReaderTheme::Dark;
    bool books_paged = true;
    bool articles_paged = false;
    int cursor_speed = 100;  // percent of 900 pixels/second at full stick
    int pdf_fit = 1;         // 0 fit page, 1 fit width, 2 custom/actual size
    int pdf_zoom = 100;      // 100% = 96 screen pixels per 72 PDF points
    int pdf_rotation = 0;    // clockwise quarter turns
    bool pdf_continuous = false;
    int pdf_background = 0;  // dark, grey, sepia, black
    // Which format to download from Calibre-Web when a book has several.
    std::string format_order = "epub,cbz,pdf,cbr,azw3,mobi,fb2,txt";
    // More OPDS catalogues, shown in the Books section after Calibre-Web.
    struct OpdsCatalog {
        std::string name, url, user, password;
    };
    std::vector<OpdsCatalog> opds_catalogs;
    std::string books_catalog;  // address of the catalogue last shown in Books
    // LibriVox's free audiobooks in the Audiobooks section (which then shows
    // even without an Audiobookshelf server).
    bool librivox = false;
};

Settings& settings();
void settings_load();
bool settings_save();
// Pushes connection settings to the HTTP layer (credentials, TLS policy).
void settings_apply();

}  // namespace svc
