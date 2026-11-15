// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Librarian: Kiwix, Calibre-Web and local documents on the PS4.
#include <stdio.h>
#include <string.h>

#include "audio/player.h"
#include "core/fs.h"
#include "services/audiobookshelf.h"
#include "services/listening.h"
#include "core/log.h"
#include "core/usb_ftp.h"
#include "core/worker.h"
#include "doc/covers.h"
#include "doc/epub.h"
#include "doc/mobi.h"
#include "doc/office.h"
#include "gfx/font.h"
#include "net/http.h"
#include "platform/platform.h"
#include "services/library.h"
#include "services/local_library.h"
#include "services/settings.h"
#include "services/updater.h"
#include "services/zim_downloads.h"
#include "services/zim_shelf.h"
#include "ui/app.h"
#include "ui/i18n.h"

#ifdef LIB_HOST
#include "platform/host.h"
#endif

int main(int argc, char** argv) {
#ifdef LIB_HOST
    // librarian [--script "steps"] [--shots dir]
    std::string script, shots = "build/host/shots";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--script")) script = argv[++i];
        else if (!strcmp(argv[i], "--shots")) shots = argv[++i];
    }
#endif
    if (!platform::init()) {
        LOG("platform init failed");
        platform::exit_app();
    }
    // A crash writes <data>/crash.log; the next start reports it in the log.
    lib::log_previous_crash(lib::join_path(platform::data_root(), "crash.log"));
    platform::install_crash_handler();
#ifdef LIB_HOST
    host::set_script(script, shots);
#endif
    LOG("Librarian %s, %s",
#ifdef LIB_VERSION
        LIB_VERSION,
#else
        "dev",
#endif
        platform::memory_report().c_str());
    if (!gfx::fonts_init(lib::join_path(platform::assets_root(), "fonts"))) LOG("some fonts are missing");
    // After the package's fonts are loaded: stepping out of the sandbox can
    // change what /app0 means.
    platform::enable_usb_access();
    if (!lib::http_init()) LOG("network init failed; servers are unavailable");
    // USB drives read through GoldHEN's FTP server: say at once whether it answers.
    if (lib::usb_ftp::enabled()) lib::usb_ftp::reachable();
    lib::make_dirs(lib::join_path(platform::data_root(), "books"));
    svc::settings_load();
    ui::language_apply();
    if (!platform::video_start(svc::settings().resolution)) {
        LOG("video could not start");
        platform::exit_app();
    }
    svc::settings_apply();
    svc::abs::settings_changed();  // (makes this console's device id the first time)
    svc::library().load();
    doc::register_archive_images();
    doc::register_cover_loader();
    doc::register_mobi_images();
    doc::register_rtf_images();
    svc::abs::register_cover_loader();
    lib::workers_start(4);
    // Finds documents on the console and USB drives from the start, so they are
    // ready before the Local tab is opened.
    svc::local_library().start();
    // ZIM files on the console and USB drives, read without a Kiwix Server.
    svc::zim_shelf().start();
    svc::zim_shelf().scan_async();
    svc::listening().start();
    // Is there a newer Librarian? Asked once, after start-up has settled.
    svc::updater::check(30000);

    ui::app().run();
    // Screens save their place and stop what they started (a book read aloud)
    // now, not during static destruction, after the player is gone.
    ui::app().close_all();

    svc::library().save_if_dirty();
    // Save where the audiobook was, and give its session a moment to close.
    svc::listening().stop();
    svc::zim_downloads().stop();
    svc::updater::shutdown();  // a collection's .part file stays, to be continued
    for (int i = 0; i < 300 && lib::jobs_in_flight() > 0; ++i) {
        lib::run_completions();
        platform::sleep_ms(10);
    }
    audio::player().shutdown();
    lib::workers_stop();
    lib::http_shutdown();
    platform::shutdown();
    platform::exit_app();
}
