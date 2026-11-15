// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Settings: a rail of categories, the category's options, and a side panel
// with help, connection status and troubleshooting steps.
#include <algorithm>
#include <functional>

#include "core/fs.h"
#include "core/text.h"
#include "core/log.h"
#include "core/usb_ftp.h"
#include "gfx/font.h"
#include "net/http.h"
#include "net/url.h"
#include "platform/platform.h"
#include "services/audiobookshelf.h"
#include "services/calibre.h"
#include "services/kiwix.h"
#include "services/listening.h"
#include "services/library.h"
#include "services/local_files.h"
#include "core/worker.h"
#include "services/calibre_session.h"
#include "services/opds.h"
#include "services/settings.h"
#include "services/updater.h"
#include "services/zim_downloads.h"
#include "ui/app.h"
#include "ui/i18n.h"
#include "ui/overlays.h"
#include "ui/troubleshoot.h"
#include "ui/widgets.h"

#ifndef LIB_VERSION
#define LIB_VERSION "dev"
#endif

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

// One page per section of the app, then how Librarian reads and looks, the
// console side (storage, USB, network) and About.
enum class Category { Knowledge, Books, Audiobooks, Reading, Appearance, System, About, Count };
const char* kCategories[] = {"Knowledge", "Books", "Audiobooks", "Reading", "Appearance", "System", "About"};

// The last connection test of a server.
struct TestState {
    bool ran = false, running = false;
    int status = 0;
    std::string error, summary;
    bool ok() const { return ran && !running && error.empty() && status >= 200 && status < 300; }
};

TestState g_tests[3];  // Kiwix, Calibre-Web, Audiobookshelf; kept while the app runs

int service_index(Service s) { return s == Service::Kiwix ? 0 : s == Service::Calibre ? 1 : 2; }
Service service_of(int i) { return i == 0 ? Service::Kiwix : i == 1 ? Service::Calibre : Service::Audiobookshelf; }

class SettingsScreen : public Screen {
public:
    SettingsScreen() {
        build();
        // Show where each server stands as soon as Settings opens.
        if (!svc::settings().kiwix_url.empty() && !g_tests[0].ran) test(Service::Kiwix);
        if (!svc::settings().calibre_url.empty() && !g_tests[1].ran) test(Service::Calibre);
        if (svc::abs::configured() && !g_tests[2].ran) test(Service::Audiobookshelf);
    }

    Tab tab() const override { return Tab::Settings; }
    bool animating() const override {
        const auto u = svc::updater::state();
        return g_tests[0].running || g_tests[1].running || g_tests[2].running ||
               u == svc::updater::State::Checking || u == svc::updater::State::Downloading;
    }

    std::vector<Hint> hints() const override {
        if (in_rail_) return {{kCross, "Open"}, {kL2, "Previous / next"}};
        const Row& r = rows_[nav_.index];
        std::vector<Hint> h = {{kCircle, "Back"}};
        if (r.adjust) h.insert(h.begin(), {kLeft | kRight, "Change"});
        else if (r.activate) h.insert(h.begin(), {kCross, "Select"});
        return h;
    }

    void update(const Input& in) override {
        if (in.press(kL2) || in.press(kR2)) {
            const int n = (int)Category::Count;
            set_category((Category)(((int)category_ + (in.press(kR2) ? 1 : n - 1)) % n));
            return;
        }
        if (in_rail_) {
            int c = (int)category_;
            if (in.repeat(kUp) && c > 0) set_category((Category)(c - 1));
            if (in.repeat(kDown) && c + 1 < (int)Category::Count) set_category((Category)(c + 1));
            if (in.press(kRight) || in.press(kCross)) enter_rows();
            return;
        }
        if (in.press(kCircle)) {
            in_rail_ = true;
            return;
        }
        const int before = nav_.index;
        nav_.update(in, (int)rows_.size(), kVisible, false);
        skip_headers(nav_.index >= before ? 1 : -1);
        Row& r = rows_[nav_.index];
        if (r.adjust) {
            if (in.repeat(kLeft)) change(r, -1);
            if (in.repeat(kRight)) change(r, 1);
        } else if (in.press(kLeft)) {
            in_rail_ = true;
            return;
        }
        if (in.press(kCross)) {
            if (r.activate) r.activate();
            else if (r.adjust) change(r, 1);
        }
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        // Rail.
        int y = kContentTop + 4;
        for (int i = 0; i < (int)Category::Count; ++i) {
            const bool on = i == (int)category_;
            const Rect r{kMarginX, y, kRailW, 72};
            if (on) c.fill_rounded(r, 14, in_rail_ ? t.accent : t.surface_hi);
            gfx::draw_text(c, on ? Face::SansSemiBold : Face::Sans, 29, r.x + 28, r.y + 17, T(kCategories[i]),
                           on && in_rail_ ? t.accent_text : on ? t.text : t.muted);
            y += 82;
        }
        // Rows.
        const int x = kMarginX + kRailW + 44, w = kRowsW;
        y = kContentTop;
        for (int i = nav_.top; i < std::min((int)rows_.size(), nav_.top + kVisible); ++i) {
            const Row& r = rows_[i];
            if (r.header) {
                gfx::draw_text(c, Face::SansSemiBold, 23, x + 6, y + 30, T(r.label), t.accent);
                y += kRowH;
                continue;
            }
            const bool on = !in_rail_ && i == nav_.index;
            const Rect box{x, y, w, kRowH - 10};
            draw_panel(c, box, on);
            if (on) draw_focus(c, box, 14);
            gfx::draw_text(c, Face::Sans, 27, box.x + 26, box.y + 16, T(r.label), t.text);
            std::string v = r.value ? T(r.value()) : "";
            const gfx::Color vc = r.value_color ? r.value_color() : (on ? t.text : t.muted);
            if (r.adjust) v = "\xE2\x80\xB9  " + v + "  \xE2\x80\xBA";
            v = gfx::ellipsize(Face::SansSemiBold, 25, v, w / 2);
            gfx::draw_text(c, Face::SansSemiBold, 25, box.right() - 26 - gfx::text_width(Face::SansSemiBold, 25, v), box.y + 18, v, vc);
            y += kRowH;
        }
        draw_scrollbar(c, {x + w + 14, kContentTop, 6, kVisible * kRowH}, nav_.top, kVisible, (int)rows_.size());
        draw_side(c, {x + w + 44, kContentTop, kWidth - kMarginX - (x + w + 44), kFooterTop - kContentTop - 20});
    }

private:
    struct Row {
        std::string label, help;
        std::function<std::string()> value;
        std::function<void()> activate;
        std::function<void(int)> adjust;
        bool header = false;
        int service = -1;  // 0 Kiwix, 1 Calibre-Web: the side panel shows its connection
        std::function<gfx::Color()> value_color;
    };
    static constexpr int kRowH = 86, kVisible = 9, kRailW = 300, kRowsW = 860;

    static Row header(const std::string& label) {
        Row r;
        r.label = label;
        r.header = true;
        return r;
    }

    // ---- the side panel -------------------------------------------------------------

    void draw_side(gfx::Canvas& c, const Rect& area) {
        const Theme& t = theme();
        draw_panel(c, area);
        const int x = area.x + 30, w = area.w - 60;
        int y = area.y + 30;
        const Row* r = in_rail_ || rows_.empty() ? nullptr : &rows_[nav_.index];
        if (r) {
            y += gfx::draw_wrapped(c, Face::SansSemiBold, 27, x, y, w, T(r->label), t.text, 2) + 10;
            const bool server_page = category_ == Category::Knowledge || category_ == Category::Books ||
                                     category_ == Category::Audiobooks;
            const int lines = server_page ? 7 : std::max(7, (area.bottom() - 30 - y) / 31);
            y += gfx::draw_wrapped(c, Face::Sans, 22, x, y, w, T(r->help), t.muted, lines, 31) + 20;
        }
        // The connection of this page's server.
        const int page_service = category_ == Category::Knowledge ? 0
                                 : category_ == Category::Books   ? 1
                                 : category_ == Category::Audiobooks ? 2
                                                                     : -1;
        if (page_service < 0) return;
        for (int s : {page_service}) {
            if (r && r->service >= 0 && r->service != s) continue;
            const TestState& st = g_tests[s];
            const std::string url = s == 0 ? svc::settings().kiwix_url : s == 1 ? svc::settings().calibre_url : svc::settings().abs_url;
            if (url.empty()) continue;
            if (y > area.bottom() - 80) break;
            c.fill({x, y, w, 2}, t.divider);
            y += 18;
            gfx::draw_text(c, Face::SansSemiBold, 22, x, y, s == 0 ? "KIWIX SERVER" : s == 1 ? "CALIBRE-WEB" : "AUDIOBOOKSHELF", t.faint);
            y += 36;
            if (st.running) {
                draw_spinner(c, x + 14, y + 16, 12);
                gfx::draw_text(c, Face::Sans, 24, x + 40, y, "Checking the connection\xE2\x80\xA6", t.muted);
                y += 48;
                continue;
            }
            if (!st.ran) {
                gfx::draw_text(c, Face::Sans, 24, x, y, "Not checked yet", t.muted);
                y += 48;
                continue;
            }
            if (st.ok()) {
                y += gfx::draw_wrapped(c, Face::SansSemiBold, 24, x, y, w, "Connected. " + st.summary, t.success, 3, 32) + 16;
                continue;
            }
            const Diagnosis d = diagnose(service_of(s), st.status, st.error);
            y += gfx::draw_wrapped(c, Face::SansSemiBold, 24, x, y, w, d.title, t.error, 3, 32) + 6;
            // What actually happened, in the server's or the network's words.
            if (!st.error.empty() && st.error != d.title)
                y += gfx::draw_wrapped(c, Face::Sans, 20, x, y, w, st.error, t.muted, 3, 27) + 10;
            for (size_t i = 0; i < d.steps.size() && y < area.bottom() - 60; ++i) {
                const std::string n = std::to_string(i + 1) + ".";
                gfx::draw_text(c, Face::SansSemiBold, 21, x, y, n, t.muted);
                y += gfx::draw_wrapped(c, Face::Sans, 21, x + 30, y, w - 30, d.steps[i], t.text,
                                       std::max(1, (area.bottom() - 40 - y) / 29), 29) + 10;
            }
            y += 6;
        }
    }

    // ---- navigation -----------------------------------------------------------------

    void set_category(Category c) {
        category_ = c;
        nav_ = ListNav();
        build();
    }

    void enter_rows() {
        in_rail_ = false;
        nav_.index = 0;
        skip_headers(1);
    }

    void skip_headers(int direction) {
        if (rows_.empty()) return;
        int i = nav_.index;
        while (i >= 0 && i < (int)rows_.size() && rows_[i].header) i += direction;
        if (i < 0 || i >= (int)rows_.size()) {
            i = nav_.index;
            while (i >= 0 && i < (int)rows_.size() && rows_[i].header) i -= direction;
        }
        nav_.index = std::max(0, std::min(i, (int)rows_.size() - 1));
        nav_.clamp((int)rows_.size(), kVisible);
        // Keep a section's header in view with its first row.
        if (nav_.index > 0 && rows_[nav_.index - 1].header && nav_.top == nav_.index) --nav_.top;
    }

    void change(Row& r, int d) {
        r.adjust(d);
        svc::settings_save();
        svc::settings_apply();
    }

    // ---- OPDS catalogues -------------------------------------------------------------

    void rebuild_keep_place() {
        const int keep = nav_.index;
        build();
        nav_.index = std::min(keep, std::max(0, (int)rows_.size() - 1));
        nav_.clamp((int)rows_.size(), kVisible);
    }

    void save_catalogs() {
        svc::settings_save();
        svc::settings_apply();
        rebuild_keep_place();
    }

    void catalog_menu(size_t i) {
        svc::Settings& s = svc::settings();
        if (i >= s.opds_catalogs.size()) return;
        std::vector<MenuItem> items;
        items.push_back({"Name", [&s, i] { return i < s.opds_catalogs.size() ? s.opds_catalogs[i].name : std::string(); },
                         [this, i] {
                             auto& c = svc::settings().opds_catalogs[i];
                             open_keyboard("Catalogue name", c.name, KeyboardKind::Text, [this, i](const std::string& v) {
                                 if (i < svc::settings().opds_catalogs.size()) svc::settings().opds_catalogs[i].name = lib::trim(v);
                                 save_catalogs();
                             });
                         },
                         nullptr});
        items.push_back({"Address", [&s, i] { return i < s.opds_catalogs.size() ? s.opds_catalogs[i].url : std::string(); },
                         [this, i] {
                             auto& c = svc::settings().opds_catalogs[i];
                             open_keyboard("Catalogue address", c.url, KeyboardKind::Url, [this, i](const std::string& v) {
                                 svc::Settings& st = svc::settings();
                                 if (i < st.opds_catalogs.size() && !lib::trim(v).empty()) {
                                     if (st.books_catalog == st.opds_catalogs[i].url) st.books_catalog = lib::trim(v);
                                     st.opds_catalogs[i].url = lib::trim(v);
                                 }
                                 save_catalogs();
                             });
                         },
                         nullptr});
        items.push_back({"User name (if it asks)",
                         [&s, i] {
                             return i < s.opds_catalogs.size() && !s.opds_catalogs[i].user.empty() ? s.opds_catalogs[i].user
                                                                                                  : std::string("None");
                         },
                         [this, i] {
                             auto& c = svc::settings().opds_catalogs[i];
                             open_keyboard("User name", c.user, KeyboardKind::Text, [this, i](const std::string& v) {
                                 if (i < svc::settings().opds_catalogs.size()) svc::settings().opds_catalogs[i].user = lib::trim(v);
                                 save_catalogs();
                             });
                         },
                         nullptr});
        items.push_back({"Password",
                         [&s, i] {
                             return i < s.opds_catalogs.size() && !s.opds_catalogs[i].password.empty() ? std::string(8, '*')
                                                                                                      : std::string("None");
                         },
                         [this, i] {
                             open_keyboard("Password", "", KeyboardKind::Password, [this, i](const std::string& v) {
                                 if (i < svc::settings().opds_catalogs.size()) svc::settings().opds_catalogs[i].password = v;
                                 save_catalogs();
                             });
                         },
                         nullptr});
        MenuItem remove{"Remove this catalogue", nullptr, [this, i] {
                            auto& list = svc::settings().opds_catalogs;
                            if (i < list.size()) list.erase(list.begin() + (long)i);
                            save_catalogs();
                        },
                        nullptr};
        remove.destructive = true;
        items.push_back(remove);
        const auto& c = s.opds_catalogs[i];
        open_menu(c.name.empty() ? "Catalogue" : c.name, std::move(items), c.url);
    }

    void step_resolution(svc::Settings& s, int d) {
        static const int kChoices[] = {0, 720, 1080, 2160};
        int i = 0;
        for (int k = 0; k < 4; ++k)
            if (kChoices[k] == s.resolution) i = k;
        s.resolution = kChoices[(i + 4 + d) % 4];
        svc::settings_save();
        app().toast("Takes effect the next time Librarian starts");
    }

    static void toggle_debug(svc::Settings& s) {
        if (s.debug_log) LOG("debug logging turned off");
        s.debug_log = !s.debug_log;
        lib::log_set_debug(s.debug_log);
        if (s.debug_log) LOG("debug logging turned on");
        svc::settings_save();
    }

    static std::string update_value() {
        using S = svc::updater::State;
        switch (svc::updater::state()) {
            case S::Idle: return T("Check now");
            case S::Checking: return T("Checking\xE2\x80\xA6");
            case S::Current: return std::string(T("Up to date")) + " (" + svc::updater::current_version() + ")";
            case S::Available: return svc::updater::available_version() + " " + T("available");
            case S::Downloading: {
                uint64_t done = 0, total = 0;
                svc::updater::progress(done, total);
                return total ? std::to_string((int)(done * 100 / total)) + "%" : lib::format_bytes(done);
            }
            case S::Downloaded: return T("Ready to install");
            case S::Failed: return T("Failed: select to try again");
        }
        return std::string();
    }

    static void update_action() {
        using S = svc::updater::State;
        switch (svc::updater::state()) {
            case S::Available:
                confirm("Download Librarian " + svc::updater::available_version() + "?",
                        "It is saved to /data/pkg/Librarian.pkg and checked; then install it from Debug Settings "
                        "\xE2\x80\xBA Game \xE2\x80\xBA Package Installer.",
                        "Download", [] { svc::updater::download(); });
                return;
            case S::Downloading:
                confirm("Stop the download?", "", "Stop", [] { svc::updater::cancel(); });
                return;
            case S::Downloaded:
                app().toast("Install it from Debug Settings \xE2\x80\xBA Game \xE2\x80\xBA Package Installer");
                return;
            case S::Checking:
                return;
            case S::Failed:
                app().toast(svc::updater::error());
                svc::updater::check();
                return;
            default:
                svc::updater::check();
        }
    }

    void check_helper() {
        app().toast("Asking the download helper\xE2\x80\xA6");
        lib::async<std::string>(
            token_,
            []() {
                svc::kiwix_helper::Status st;
                std::string error;
                if (!svc::kiwix_helper::status(st, error)) return "The helper did not answer: " + error;
                std::string out = "The helper answers. It saves to " + st.folder;
                if (st.free_bytes) out += " (" + lib::format_bytes(st.free_bytes) + " free)";
                out += st.updates_library ? ", and adds collections to the server itself." : ".";
                return out;
            },
            [](std::string& text) { app().toast(text); });
    }

    void edit_text(const std::string& title, std::string* target, KeyboardKind kind, int service) {
        // A password is typed afresh: what is stored shows only as dots, so
        // editing it would keep mistakes no one can see.
        const std::string initial = kind == KeyboardKind::Password ? std::string()
                                    : target->empty() && kind == KeyboardKind::Url ? "http://"
                                                                                   : *target;
        open_keyboard(title, initial, kind, [this, target, kind, service](const std::string& v) {
            // Spaces around an address or a user name are never meant; a
            // password is kept exactly as typed.
            *target = kind == KeyboardKind::Url ? lib::normalise_server_url(lib::trim(v))
                      : kind == KeyboardKind::Password ? v
                                                       : lib::trim(v);
            svc::settings_save();
            svc::settings_apply();
            if (service == 1) svc::calibre::forget_session();  // new credentials -> new session
            if (service == 2) svc::abs::settings_changed();
            // A changed address or sign-in is checked straight away.
            if (service >= 0) test(service_of(service));
        });
    }

    // ---- connection tests -----------------------------------------------------------

    void test(Service service) {
        const int s = service_index(service);
        const std::string url = lib::normalise_server_url(s == 0 ? svc::settings().kiwix_url
                                                          : s == 1 ? svc::settings().calibre_url
                                                                   : svc::settings().abs_url);
        if (url.empty()) return;
        g_tests[s].running = true;
        struct Result {
            int status = -1;
            std::string error, summary;
        };
        // Not tied to this screen: the result is kept for the next visit.
        lib::async<Result>(
            nullptr,
            [s, url]() {
                Result r;
                if (s == 2) {
                    // Signs in, and counts libraries and books.
                    const svc::abs::Status st = svc::abs::test_connection(r.summary);
                    r.status = st.status;
                    r.error = st.error;
                    if (st.ok() && !r.status) r.status = 200;
                } else if (s == 0) {
                    std::vector<svc::ZimBook> books;
                    if (svc::kiwix::catalog(url, books, r.error, &r.status))
                        r.summary = std::to_string(books.size()) + " collections.";
                    else if (r.status >= 200 && r.status < 300 && r.error.empty())
                        r.error = "The server lists no collections.";
                } else {
                    svc::OpdsFeed feed;
                    if (svc::opds_fetch(url + "/opds", feed, r.error, &r.status)) {
                        r.summary = feed.title.empty() ? "The catalogue is available." : feed.title + ": the catalogue is available.";
                        if (svc::settings().calibre_anonymous) r.summary += " Browsing anonymously.";
                    } else if (r.status >= 200 && r.status < 300 && r.error.empty()) {
                        r.error = "Not an OPDS catalogue.";
                    }
                }
                return r;
            },
            [s](Result& r) {
                TestState& st = g_tests[s];
                st.ran = true;
                st.running = false;
                st.status = r.status;
                st.error = r.error;
                st.summary = r.summary;
            });
    }

    std::function<std::string()> test_value(int s) {
        return [s] {
            const TestState& st = g_tests[s];
            if (st.running) return std::string("Checking\xE2\x80\xA6");
            if (!st.ran) return std::string("Check now");
            return std::string(st.ok() ? "Connected" : "Not connected");
        };
    }

    std::function<gfx::Color()> test_color(int s) {
        return [s] {
            const TestState& st = g_tests[s];
            if (!st.ran || st.running) return theme().muted;
            return st.ok() ? theme().success : theme().error;
        };
    }

    // ---- rows -----------------------------------------------------------------------

    void build() {
        svc::Settings& s = svc::settings();
        rows_.clear();
        switch (category_) {
            case Category::Knowledge: {
                rows_.push_back(header("KIWIX SERVER"));
                Row kurl{"Address",
                         "Where your Kiwix Server is, with its port: for example http://192.168.1.10:8080. Use the IP "
                         "address; names ending in .local do not work on the PS4.",
                         [&s] { return s.kiwix_url.empty() ? std::string("Not set") : s.kiwix_url; },
                         [this, &s] { edit_text("Kiwix Server address", &s.kiwix_url, KeyboardKind::Url, 0); }, nullptr};
                kurl.service = 0;
                rows_.push_back(kurl);
                Row ktest{"Connection", "Checks that Kiwix Server answers and lists its collections.", test_value(0),
                          [this] { test(Service::Kiwix); }, nullptr};
                ktest.service = 0;
                ktest.value_color = test_color(0);
                rows_.push_back(ktest);
                rows_.push_back(header("KIWIX ONLINE LIBRARY"));
                rows_.push_back({"Kiwix online library",
                                 "Shows the Kiwix online library (library.kiwix.org) in the Knowledge section, also without "
                                 "a Kiwix Server: find collections and download them to a USB drive, the console, or (with "
                                 "the download helper) your server. The online library is always reachable from the "
                                 "Knowledge section's first card when a server is set.",
                                 [&s] { return std::string(s.kiwix_online ? "Shown" : "Only with a server"); },
                                 [&s] {
                                     s.kiwix_online = !s.kiwix_online;
                                     svc::settings_save();
                                 },
                                 [&s](int) { s.kiwix_online = !s.kiwix_online; }});
                rows_.push_back({"Download helper",
                                 "A small program that runs beside your Kiwix Server (extras/kiwix-helper in Librarian's "
                                 "source) and downloads collections from the online library into the server's folder, so "
                                 "the PS4 does not have to. Its address, for example http://192.168.1.10:8091.",
                                 [&s] { return s.kiwix_helper_url.empty() ? std::string("Not set") : s.kiwix_helper_url; },
                                 [this, &s] { edit_text("Download helper address", &s.kiwix_helper_url, KeyboardKind::Url, -1); },
                                 nullptr});
                {
                    rows_.push_back({"Helper key",
                                     "The key the helper printed when it started (also in its key file). Without it, the "
                                     "helper refuses requests.",
                                     [&s] { return s.kiwix_helper_key.empty() ? std::string("Not set") : std::string("Set"); },
                                     [this, &s] { edit_text("Helper key", &s.kiwix_helper_key, KeyboardKind::Text, -1); },
                                     nullptr});
                    rows_.push_back({"Check the helper", "Asks the helper whether it answers, and where it saves collections.",
                                     nullptr, [this] { check_helper(); }, nullptr});
                }

                break;
            }
            case Category::Books: {
                rows_.push_back(header("CALIBRE-WEB"));
                Row curl{"Address",
                         "Where Calibre-Web is: for example http://192.168.1.10:8083. Librarian reads its OPDS catalogue.",
                         [&s] { return s.calibre_url.empty() ? std::string("Not set") : s.calibre_url; },
                         [this, &s] { edit_text("Calibre-Web address", &s.calibre_url, KeyboardKind::Url, 1); }, nullptr};
                curl.service = 1;
                rows_.push_back(curl);
                Row mode{"Sign-in",
                         "Anonymous: no account is used, for servers with anonymous browsing enabled. With an account: "
                         "your Calibre-Web user name and password are sent to this server only.",
                         [&s] { return std::string(s.calibre_anonymous ? "Anonymous" : "With an account"); }, nullptr,
                         [this, &s](int) {
                             s.calibre_anonymous = !s.calibre_anonymous;
                             svc::calibre::forget_session();
                             const int keep = nav_.index;
                             build();
                             nav_.index = keep;
                             nav_.clamp((int)rows_.size(), kVisible);
                             svc::settings_apply();
                             if (!s.calibre_url.empty() && (s.calibre_anonymous || !s.calibre_user.empty()))
                                 test(Service::Calibre);
                             else
                                 g_tests[1] = TestState();  // waits for a user name
                         }};
                mode.service = 1;
                rows_.push_back(mode);
                if (!s.calibre_anonymous) {
                    Row user{"User name", "Your Calibre-Web user name.",
                             [&s] { return s.calibre_user.empty() ? std::string("Not set") : s.calibre_user; },
                             [this, &s] { edit_text("Calibre-Web user name", &s.calibre_user, KeyboardKind::Text, 1); }, nullptr};
                    user.service = 1;
                    rows_.push_back(user);
                    Row pass{"Password", "Stored only on this console, in Librarian's private folder, and sent only to Calibre-Web.",
                             [&s] { return s.calibre_password.empty() ? std::string("Not set") : std::string(8, '*'); },
                             [this, &s] { edit_text("Calibre-Web password", &s.calibre_password, KeyboardKind::Password, 1); },
                             nullptr};
                    pass.service = 1;
                    rows_.push_back(pass);
                }
                Row ctest{"Connection", "Checks that Calibre-Web answers and its OPDS catalogue can be read.", test_value(1),
                          [this] { test(Service::Calibre); }, nullptr};
                ctest.service = 1;
                ctest.value_color = test_color(1);
                rows_.push_back(ctest);
                if (!s.calibre_anonymous) {
                    Row signin{"Test sign-in",
                               "Signs in with the account above (needed only for uploading books or seeing private "
                               "shelves). Calibre-Web limits sign-in attempts, so try sparingly.",
                               nullptr, [] {
                                   app().toast("Signing in…");
                                   lib::async<svc::calibre::SessionStatus>(
                                       nullptr, [] { return svc::calibre::login_from_settings(); },
                                       [](svc::calibre::SessionStatus& r) { app().toast(r.message); });
                               },
                               nullptr};
                    signin.service = 1;
                    rows_.push_back(signin);
                }
                Row formats{"Preferred format",
                            "When a book is in several formats, the first one here that Librarian reads is downloaded.",
                            [&s] {
                                std::string f = s.format_order.substr(0, s.format_order.find(','));
                                for (char& ch : f) ch = (char)toupper((unsigned char)ch);
                                return f + " first";
                            },
                            nullptr, [&s](int d) {
                                static const char* orders[] = {"epub,cbz,pdf,cbr,azw3,mobi,fb2,txt", "pdf,epub,cbz,cbr,azw3,mobi,fb2,txt", "cbz,cbr,epub,pdf,azw3,mobi,fb2,txt"};
                                int i = 0;
                                for (int k = 0; k < 3; ++k)
                                    if (s.format_order == orders[k]) i = k;
                                s.format_order = orders[(i + 3 + d) % 3];
                            }};
                formats.service = 1;
                rows_.push_back(formats);

                rows_.push_back(header("OPDS CATALOGUES"));
                for (size_t i = 0; i < s.opds_catalogs.size(); ++i) {
                    const auto& cat = s.opds_catalogs[i];
                    rows_.push_back({cat.name.empty() ? cat.url : cat.name,
                                     "An OPDS catalogue, shown in the Books section after Calibre-Web (L2 / R2 there "
                                     "switch between catalogues). Select to change or remove it.",
                                     [&s, i] { return i < s.opds_catalogs.size() ? s.opds_catalogs[i].url : std::string(); },
                                     [this, i] { catalog_menu(i); }, nullptr});
                }
                rows_.push_back({"Add a catalogue",
                                 "Any OPDS 1 catalogue: another Calibre-Web or Calibre content server, a public library "
                                 "such as Project Gutenberg, or your own. Its books can be browsed, searched (when it "
                                 "offers a search) and downloaded like Calibre-Web's.",
                                 nullptr, [this] { open_add_catalogue_menu([this] { rebuild_keep_place(); }); }, nullptr});

                break;
            }
            case Category::Audiobooks: {
                rows_.push_back(header("LIBRIVOX"));
                rows_.push_back({"Free audiobooks",
                                 "LibriVox's public-domain audiobooks, read by volunteers, in the Audiobooks section (which "
                                 "then shows even without an Audiobookshelf server): new books, search, streaming from "
                                 "archive.org, and downloads for listening offline.",
                                 [&s] { return std::string(s.librivox ? "Shown" : "Hidden"); },
                                 [&s] {
                                     s.librivox = !s.librivox;
                                     svc::settings_save();
                                 },
                                 [&s](int) { s.librivox = !s.librivox; }});

                rows_.push_back(header("AUDIOBOOKSHELF"));
                Row aurl{"Address",
                         "Where Audiobookshelf is: for example http://192.168.1.10:13378. Its audiobooks then appear in "
                         "the Audiobooks section.",
                         [&s] { return s.abs_url.empty() ? std::string("Not set") : s.abs_url; },
                         [this, &s] { edit_text("Audiobookshelf address", &s.abs_url, KeyboardKind::Url, 2); }, nullptr};
                aurl.service = 2;
                rows_.push_back(aurl);
                Row auser{"User name", "Your Audiobookshelf user name (as in its web page).",
                          [&s] { return s.abs_user.empty() ? std::string("Not set") : s.abs_user; },
                          [this, &s] { edit_text("Audiobookshelf user name", &s.abs_user, KeyboardKind::Text, 2); }, nullptr};
                auser.service = 2;
                rows_.push_back(auser);
                Row apass{"Password",
                          "Stored only on this console, in Librarian's private folder, and sent only to Audiobookshelf when "
                          "signing in. Afterwards Librarian uses the sign-in tokens the server gives it.",
                          [&s] { return s.abs_password.empty() ? std::string("Not set") : std::string(8, '*'); },
                          [this, &s] { edit_text("Audiobookshelf password", &s.abs_password, KeyboardKind::Password, 2); },
                          nullptr};
                apass.service = 2;
                rows_.push_back(apass);
                Row akey{"API key (optional)",
                         "Instead of a password: a key made in Audiobookshelf's Settings › API Keys (version 2.26 or newer). "
                         "When set, it is used rather than the user name and password.",
                         [&s] { return s.abs_api_key.empty() ? std::string("Not set") : std::string("Set"); },
                         [this, &s] { edit_text("Audiobookshelf API key", &s.abs_api_key, KeyboardKind::Password, 2); }, nullptr};
                akey.service = 2;
                rows_.push_back(akey);
                Row atest{"Connection", "Signs in and counts the audiobook libraries and books.", test_value(2),
                          [this] { test(Service::Audiobookshelf); }, nullptr};
                atest.service = 2;
                atest.value_color = test_color(2);
                rows_.push_back(atest);
                break;
            }
            case Category::Reading:
                rows_.push_back({"Text size", "The size of body text. You can also change it while reading with Options.",
                                 [&s] { return std::to_string(s.font_px) + " px"; }, nullptr,
                                 [&s](int d) { s.font_px = std::max(22, std::min(64, s.font_px + 2 * d)); }});
                rows_.push_back({"Typeface", "Serif (Noto Serif) or sans serif (Noto Sans) for body text.",
                                 [&s] { return std::string(s.serif ? "Serif" : "Sans serif"); }, nullptr,
                                 [&s](int) { s.serif = !s.serif; }});
                rows_.push_back({"Line spacing", "Space between lines, as a percentage of the text size.",
                                 [&s] { return std::to_string(s.line_spacing) + "%"; }, nullptr,
                                 [&s](int d) { s.line_spacing = std::max(110, std::min(200, s.line_spacing + 10 * d)); }});
                rows_.push_back({"Margins", "How wide the column of text is. Wide margins make shorter lines.",
                                 [&s] {
                                     static const char* names[] = {"Narrow", "Normal", "Wide"};
                                     return std::string(names[s.margins]);
                                 },
                                 nullptr, [&s](int d) { s.margins = (s.margins + 3 + d) % 3; }});
                rows_.push_back({"Justify text", "Align text to both edges of the column.",
                                 [&s] { return std::string(s.justify ? "On" : "Off"); }, nullptr,
                                 [&s](int) { s.justify = !s.justify; }});
                rows_.push_back({"Books", "Turn pages, or scroll continuously, in EPUB, Kindle, FictionBook and text books.",
                                 [&s] { return std::string(s.books_paged ? "Pages" : "Scrolling"); }, nullptr,
                                 [&s](int) { s.books_paged = !s.books_paged; }});
                rows_.push_back({"Articles", "Turn pages, or scroll continuously, in Kiwix articles.",
                                 [&s] { return std::string(s.articles_paged ? "Pages" : "Scrolling"); }, nullptr,
                                 [&s](int) { s.articles_paged = !s.articles_paged; }});
                break;
            case Category::Appearance:
                rows_.push_back({"Language",
                                 "The language of Librarian's menus and messages. Auto follows the PS4's system "
                                 "language. Book titles, articles and what servers send are shown as they are.",
                                 [] { return ui::language_label(); },
                                 [] { ui::language_step(1); },
                                 [](int d) { ui::language_step(d); }});
                rows_.push_back({"Resolution",
                                 "The resolution Librarian draws at. Auto matches what the PS4 sends to the TV: 720p, "
                                 "1080p, or 2160p (4K) on a PS4 Pro. A lower one draws faster. Takes effect the next time "
                                 "Librarian starts.",
                                 [&s] {
                                     const std::string now = std::to_string(platform::frame_height()) + "p";
                                     if (!s.resolution) return std::string(T("Auto")) + " (" + now + ")";
                                     return std::to_string(s.resolution) + "p";
                                 },
                                 [this, &s] { step_resolution(s, 1); },
                                 [this, &s](int d) { step_resolution(s, d); }});
                rows_.push_back({"Interface theme", "The colours of menus and libraries.",
                                 [&s] { return std::string(s.ui_theme ? "Light" : "Dark"); }, nullptr,
                                 [&s](int) { s.ui_theme = 1 - s.ui_theme; }});
                rows_.push_back({"Reading theme",
                                 "The page colours in the reader. Sepia and Black are easier on the eyes in a dark room.",
                                 [&s] {
                                     static const char* names[] = {"Dark", "Light", "Sepia", "Black"};
                                     return std::string(names[(int)s.reader_theme]);
                                 },
                                 nullptr, [&s](int d) { s.reader_theme = (svc::ReaderTheme)(((int)s.reader_theme + 4 + d) % 4); }});
                break;
            case Category::System:
                rows_.push_back(header("STORAGE"));
                rows_.push_back({"USB drives",
                                 "How Librarian reaches USB drives. Directly when GoldHEN lifts the app's sandbox; otherwise "
                                 "through GoldHEN's FTP server on the console, which must be on in GoldHEN's settings "
                                 "(drives are then slower to scan and read).",
                                 [] {
                                     if (!lib::usb_ftp::enabled()) return std::string("Direct");
                                     const int a = lib::usb_ftp::answering();
                                     return std::string(a > 0   ? "Through GoldHEN FTP"
                                                        : a < 0 ? "GoldHEN FTP is off"
                                                                : "GoldHEN FTP");
                                 },
                                 nullptr, nullptr});
                rows_.push_back({"Download books to",
                                 "Where books from Calibre-Web and audiobooks from Audiobookshelf are saved. On USB keeps "
                                 "console storage free (audiobooks are large); if no USB drive is connected, the console is "
                                 "used instead.",
                                 [&s] {
                                     if (!s.download_to_usb) return std::string("Console storage");
                                     const std::string usb = svc::first_connected_usb();
                                     return std::string(usb.empty() ? "USB drive (none connected)" : "USB drive");
                                 },
                                 nullptr, [&s](int) { s.download_to_usb = !s.download_to_usb; }});
                rows_.push_back({"Find missing covers",
                                 "Books and audiobooks with no cover of their own get one: first a picture beside the "
                                 "file (same name, or cover.jpg / folder.jpg in a talking book's folder), then, when this "
                                 "is on, from Open Library (openlibrary.org), looked up by the book's title and author. "
                                 "Only books whose file names a title and an author are looked up; covers found are kept "
                                 "on the console.",
                                 [&s] { return std::string(s.online_covers ? "Open Library" : "Pictures beside the files"); },
                                 [&s] { s.online_covers = !s.online_covers; svc::settings_save(); },
                                 [&s](int) { s.online_covers = !s.online_covers; svc::settings_save(); }});
                rows_.push_back({"Downloaded books", "Books downloaded from catalogues, readable offline. Select to see them.",
                                 [] {
                                     uint64_t total = 0;
                                     int n = 0;
                                     std::vector<std::string> dirs = {lib::join_path(data_root(), "books")};
                                     const std::string usb = svc::first_connected_usb();
                                     if (!usb.empty()) dirs.push_back(lib::join_path(usb, "Librarian"));
                                     for (const auto& dir : dirs) {
                                         std::vector<lib::DirEntry> entries;
                                         if (lib::list_dir(dir, entries))
                                             for (const auto& e : entries)
                                                 if (!e.is_dir && !lib::ends_with(e.name, ".part")) {
                                                     total += e.size;
                                                     ++n;
                                                 }
                                     }
                                     return std::to_string(n) + (n == 1 ? " book, " : " books, ") + lib::format_bytes(total);
                                 },
                                 [] { app().switch_tab(Tab::Local); }, nullptr});
                rows_.push_back({"Downloaded audiobooks",
                                 "Audiobooks downloaded for listening offline, on the console and connected USB drives. Select "
                                 "to see them.",
                                 [] {
                                     uint64_t total = 0;
                                     for (const auto& d : svc::listening().downloads()) total += d.bytes;
                                     const size_t n = svc::listening().downloads().size();
                                     return std::to_string(n) + (n == 1 ? " audiobook, " : " audiobooks, ") + lib::format_bytes(total);
                                 },
                                 [] {
                                     if (tab_visible(Tab::Audiobooks)) app().switch_tab(Tab::Audiobooks);
                                     else app().toast("No audiobooks are downloaded.");
                                 },
                                 nullptr});
                rows_.push_back(header("NETWORK"));
                rows_.push_back({"Self-signed certificates",
                                 "Allow HTTPS servers whose certificates the console cannot verify, as many home servers use. "
                                 "Only turn this on for servers on your own network.",
                                 [&s] { return std::string(s.allow_insecure_tls ? "Allowed" : "Not allowed"); }, nullptr,
                                 [&s](int) { s.allow_insecure_tls = !s.allow_insecure_tls; }});
                rows_.push_back(header("PRIVACY"));
                rows_.push_back({"Clear reading history",
                                 "Forgets recent articles. Books, positions, bookmarks and favourites are kept.", nullptr, [] {
                                     confirm("Clear reading history?", "Recent articles will be removed from Home.", "Clear", [] {
                                         std::vector<std::string> ids;
                                         for (const auto& it : svc::library().items())
                                             if (it.kind == svc::ItemKind::Article && !it.favourite && it.bookmarks.empty())
                                                 ids.push_back(it.id);
                                         for (const auto& id : ids) svc::library().remove(id);
                                         app().toast("History cleared");
                                     });
                                 },
                                 nullptr});
                rows_.push_back(header("SOFTWARE UPDATE"));
                rows_.push_back({"Software update",
                                 "Looks for a newer Librarian among the project's releases on GitHub (also shortly after "
                                 "each start). A newer one is downloaded to /data/pkg/Librarian.pkg and checked; install it "
                                 "from Debug Settings \xE2\x80\xBA Game \xE2\x80\xBA Package Installer. Your settings "
                                 "and library are kept.",
                                 [] { return update_value(); }, [] { update_action(); }, nullptr});
                rows_.push_back(header("DIAGNOSTICS"));
                rows_.push_back({"Debug logging",
                                 "Writes every network request and other detail to librarian.log in the data folder. Turn "
                                 "it on when reporting a problem, then do the thing again. Passwords and keys are never "
                                 "logged. When off, the log keeps start-up, problems and what Librarian decided.",
                                 [&s] { return std::string(s.debug_log ? "On" : "Off"); },
                                 [&s] { toggle_debug(s); },
                                 [&s](int) { toggle_debug(s); }});
                rows_.push_back({"Data folder",
                                 "Settings (encrypted), the library database, downloads and the diagnostic log "
                                 "(librarian.log, and crash.log after a crash).",
                                 [] { return data_root(); }, nullptr, nullptr});
                break;
            case Category::About:
                rows_.push_back({"Librarian",
                                 "A reader for Kiwix collections, ebooks, comics, PDFs and audiobooks. By jw-notes: "
                                 "https://github.com/jw-notes. Copyright (C) 2026 jw-notes.",
                                 [] { return std::string("Version " LIB_VERSION); }, nullptr, nullptr});
                rows_.push_back({"Licence",
                                 "Free software under the GNU General Public License, version 3 or (at your option) any "
                                 "later version, with NO WARRANTY to the extent permitted by law. This package includes "
                                 "MuPDF (AGPL-3.0), so the package as a whole is distributed under the GNU Affero General "
                                 "Public License version 3. The licences, NOTICE and the complete source come with each "
                                 "release.",
                                 [] { return std::string("GPL-3.0+ \xC2\xB7 AGPL-3.0"); }, nullptr, nullptr});
                rows_.push_back({"Terms",
                                 "Additional terms (GPLv3 section 7, see NOTICE): any copy, port or modified version must "
                                 "keep these notices, credit \"Based on Librarian by jw-notes - https://github.com/jw-notes\" "
                                 "in its README and About screen, keep the credits to every other author, and must not be "
                                 "presented as original work.",
                                 [] { return std::string("NOTICE"); }, nullptr, nullptr});
                rows_.push_back({"Credits",
                                 "USB drives are reached with GoldHEN by SiSTR0 and the GoldHEN team: its Plugins SDK "
                                 "(MIT) lifts the sandbox, and where it cannot, drives are read through GoldHEN's FTP "
                                 "server. Built with the OpenOrbis PS4 Toolchain (GPL 3). Free libraries from Kiwix, "
                                 "Project Gutenberg and LibriVox. Not made or endorsed by Sony.",
                                 [] { return std::string("GoldHEN, OpenOrbis"); }, nullptr, nullptr});
                rows_.push_back({"Components",
                                 "MuPDF (PDF), libarchive (CBR), FAAD2 (AAC audio), minimp3, dr_flac, libopus and "
                                 "stb_vorbis (Opus and Vorbis audio), zstd and XZ Embedded (ZIM files), Monocypher "
                                 "(settings encryption), libwebp, stb_image, stb_image_write, stb_truetype, miniz, and "
                                 "the Noto fonts, Noto Sans JP included (SIL Open Font License). Their licences are in "
                                 "the package's licenses folder.",
                                 [] { return std::string("15 libraries, fonts"); }, nullptr, nullptr});
                rows_.push_back({"Notices", "Code from FAAD2 is copyright (c) Nero AG, www.nero.com. This software is "
                                            "based in part on the work of the Independent JPEG Group. Portions of this "
                                            "software are copyright (C) 2023 The FreeType Project (www.freetype.org). "
                                            "All rights reserved.",
                                 nullptr, nullptr, nullptr});
                rows_.push_back({"This PS4",
                                 "What Librarian is using now: the resolution it draws at, the interface language, and how "
                                 "USB drives are reached.",
                                 [] {
                                     return std::to_string(platform::frame_height()) + "p \xC2\xB7 " + ui::language_label() +
                                            (lib::usb_ftp::enabled() ? " \xC2\xB7 USB via FTP" : "");
                                 },
                                 nullptr, nullptr});
                break;
            case Category::Count:
                break;
        }
    }

    Category category_ = Category::Knowledge;
    bool in_rail_ = true;
    std::vector<Row> rows_;
    ListNav nav_;
};

}  // namespace

std::unique_ptr<Screen> make_settings_screen() { return std::make_unique<SettingsScreen>(); }

}  // namespace ui
