// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Leaving the PlayStation app sandbox so USB drives (/mnt/usb0..7) can be
// read, with GoldHEN's own escape through its Plugins SDK (MIT),
// sandbox_goldhen.cpp. When it fails, drives go through GoldHEN's FTP server
// (core/usb_ftp.h).
#include <functional>

namespace platform::sandbox {

// "GoldHEN SDK", for the log.
const char* method();
// Steps out of the sandbox. Once, on the main thread, before worker threads
// start (credentials and root directories are process-wide). Logs each step.
bool leave();
// Hands the original credentials back (on exit).
void restore();
// True while this process is outside the sandbox through leave().
bool lifted();
// Runs `fn` with the app's original credentials, then steps out again.
void with_app_credentials(const std::function<void()>& fn);

}  // namespace platform::sandbox
