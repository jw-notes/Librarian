// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/settings.h"

#include <algorithm>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "net/http.h"
#include "platform/platform.h"
#include "services/secure_store.h"

namespace svc {

namespace {
Settings g_settings;
std::string path() { return lib::join_path(platform::data_root(), "settings.json"); }
int clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
}  // namespace

Settings& settings() { return g_settings; }

void settings_load() {
    std::string text;
    const SecureRead got = read_secure(path(), text);
    if (got == SecureRead::Missing) return;
    if (got == SecureRead::Failed) {
        // Made on another console, or altered: kept aside, never overwritten.
        lib::rename_file(path(), path() + ".unreadable");
        LOG("settings: settings.json could not be decrypted (another console's, or damaged); starting afresh");
        return;
    }
    bool ok = false;
    const lib::Json j = lib::Json::parse(text, &ok);
    if (!ok) {
        LOG("settings.json is damaged; using defaults");
        return;
    }
    Settings& s = g_settings;
    const Settings d;
    s.kiwix_url = j["kiwix_url"].str();
    s.kiwix_online = j["kiwix_online"].boolean(false);
    s.kiwix_helper_url = j["kiwix_helper_url"].str();
    s.kiwix_helper_key = j["kiwix_helper_key"].str();
    s.books_catalog = j["books_catalog"].str();
    s.librivox = j["librivox"].boolean(false);
    s.calibre_url = j["calibre_url"].str();
    s.calibre_user = j["calibre_user"].str();
    s.calibre_password = j["calibre_password"].str();
    // Older settings had no switch: an empty user name meant anonymous.
    s.calibre_anonymous = j["calibre_anonymous"].boolean(s.calibre_user.empty());
    s.abs_url = j["abs_url"].str();
    s.abs_user = j["abs_user"].str();
    s.abs_password = j["abs_password"].str();
    s.abs_api_key = j["abs_api_key"].str();
    s.abs_device_id = j["abs_device_id"].str();
    s.abs_library = j["abs_library"].str();
    s.listen_speed = clamp((int)j["listen_speed"].integer(d.listen_speed), 50, 300);
    s.allow_insecure_tls = j["allow_insecure_tls"].boolean(false);
    s.download_to_usb = j["download_to_usb"].boolean(false);
    s.ui_theme = clamp((int)j["ui_theme"].integer(0), 0, 1);
    if (!j["language"].str().empty()) s.language = j["language"].str();
    s.resolution = (int)j["resolution"].integer(0);
    s.debug_log = j["debug_log"].boolean(false);
    s.online_covers = j["online_covers"].boolean(true);
    if (s.resolution != 720 && s.resolution != 1080 && s.resolution != 2160) s.resolution = 0;
    s.font_px = clamp((int)j["font_px"].integer(d.font_px), 22, 64);
    s.line_spacing = clamp((int)j["line_spacing"].integer(d.line_spacing), 110, 200);
    s.serif = j["serif"].boolean(d.serif);
    s.justify = j["justify"].boolean(d.justify);
    s.margins = clamp((int)j["margins"].integer(d.margins), 0, 2);
    s.reader_theme = (ReaderTheme)clamp((int)j["reader_theme"].integer(0), 0, 3);
    s.books_paged = j["books_paged"].boolean(d.books_paged);
    s.articles_paged = j["articles_paged"].boolean(d.articles_paged);
    s.cursor_speed = clamp((int)j["cursor_speed"].integer(d.cursor_speed), 25, 250);
    s.pdf_fit = clamp((int)j["pdf_fit"].integer(d.pdf_fit), 0, 2);
    s.pdf_zoom = clamp((int)j["pdf_zoom"].integer(d.pdf_zoom), 25, 400);
    s.pdf_rotation = clamp((int)j["pdf_rotation"].integer(d.pdf_rotation), 0, 3);
    s.pdf_continuous = j["pdf_continuous"].boolean(d.pdf_continuous);
    s.pdf_background = clamp((int)j["pdf_background"].integer(d.pdf_background), 0, 3);
    if (!j["format_order"].str().empty()) s.format_order = j["format_order"].str();
    s.opds_catalogs.clear();
    for (const lib::Json& c : j["opds_catalogs"].items()) {
        Settings::OpdsCatalog cat{c["name"].str(), c["url"].str(), c["user"].str(), c["password"].str()};
        if (!cat.url.empty()) s.opds_catalogs.push_back(cat);
    }
    if (got == SecureRead::Plain) {
        // An older, unencrypted file: replaced by the encrypted one now.
        if (settings_save()) LOG("settings: settings.json is now encrypted");
    }
}

bool settings_save() {
    const Settings& s = g_settings;
    lib::Json j = lib::Json::object();
    j.set("kiwix_url", s.kiwix_url);
    j.set("kiwix_online", s.kiwix_online);
    j.set("kiwix_helper_url", s.kiwix_helper_url);
    j.set("kiwix_helper_key", s.kiwix_helper_key);  // private to the console, never logged
    j.set("books_catalog", s.books_catalog);
    j.set("librivox", s.librivox);
    j.set("calibre_url", s.calibre_url);
    j.set("calibre_user", s.calibre_user);
    // Stored as entered: the console's /data folder is private to it. The log
    // never contains it.
    j.set("calibre_password", s.calibre_password);
    j.set("calibre_anonymous", s.calibre_anonymous);
    // Like the Calibre-Web password: private to the console, never logged.
    j.set("abs_url", s.abs_url);
    j.set("abs_user", s.abs_user);
    j.set("abs_password", s.abs_password);
    j.set("abs_api_key", s.abs_api_key);
    j.set("abs_device_id", s.abs_device_id);
    j.set("abs_library", s.abs_library);
    j.set("listen_speed", s.listen_speed);
    j.set("allow_insecure_tls", s.allow_insecure_tls);
    j.set("download_to_usb", s.download_to_usb);
    j.set("ui_theme", s.ui_theme);
    j.set("language", s.language);
    j.set("resolution", s.resolution);
    j.set("debug_log", s.debug_log);
    j.set("online_covers", s.online_covers);
    j.set("font_px", s.font_px);
    j.set("line_spacing", s.line_spacing);
    j.set("serif", s.serif);
    j.set("justify", s.justify);
    j.set("margins", s.margins);
    j.set("reader_theme", (int)s.reader_theme);
    j.set("books_paged", s.books_paged);
    j.set("articles_paged", s.articles_paged);
    j.set("cursor_speed", s.cursor_speed);
    j.set("pdf_fit", s.pdf_fit);
    j.set("pdf_zoom", s.pdf_zoom);
    j.set("pdf_rotation", s.pdf_rotation);
    j.set("pdf_continuous", s.pdf_continuous);
    j.set("pdf_background", s.pdf_background);
    j.set("format_order", s.format_order);
    lib::Json catalogs = lib::Json::array();
    for (const auto& c : s.opds_catalogs) {
        lib::Json e = lib::Json::object();
        e.set("name", c.name);
        e.set("url", c.url);
        // Like the passwords above: private to the console, never logged.
        if (!c.user.empty()) {
            e.set("user", c.user);
            e.set("password", c.password);
        }
        catalogs.push(std::move(e));
    }
    j.set("opds_catalogs", std::move(catalogs));
    // Encrypted: it holds passwords and keys (services/secure_store.h).
    const bool ok = write_secure(path(), j.dump(true));
    if (!ok) LOG("could not save settings");
    return ok;
}

void settings_apply() {
    const Settings& s = g_settings;
    lib::log_set_debug(s.debug_log);
    lib::http_set_allow_insecure(s.allow_insecure_tls);
    for (const auto& c : s.opds_catalogs) lib::http_set_credentials(c.url, c.user, c.password);
    if (s.calibre_url.empty()) return;
    if (s.calibre_anonymous || s.calibre_user.empty()) lib::http_set_credentials(s.calibre_url, "", "");
    else lib::http_set_credentials(s.calibre_url, s.calibre_user, s.calibre_password);
}

}  // namespace svc
