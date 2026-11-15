// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Diagnostic log. On the console it is written to <data>/librarian.log (the
// previous run is kept as librarian.prev.log); on the host it also goes to stderr.
// Never log passwords, tokens or Authorization headers.
#include <stdint.h>

#include <string>

namespace lib {

void log_open(const std::string& path);
void log_line(const char* format, ...) __attribute__((format(printf, 1, 2)));

// After a fatal signal (from the crash handler, platform): writes `crash_path`
// with the signal, the faulting addresses and the last lines logged, using
// only open/write, and adds the same report to the log. `main_at` is the
// address of a known function, to turn `pc` into a place in the program.
void log_crash(const char* crash_path, int signal, const char* name, uintptr_t fault, uintptr_t pc,
               uintptr_t main_at);
// At startup: if the previous run left a crash.log, logs its first lines and
// renames it to crash.prev.log. True when there was one.
bool log_previous_crash(const std::string& crash_path);

// Settings › System › Debug logging: detail lines (LOGD: every network
// request and similar) are written only when it is on. LOG lines (startup,
// problems, what Librarian decided) always are.
void log_set_debug(bool on);
bool log_debug();

}  // namespace lib

#define LOG(...) ::lib::log_line(__VA_ARGS__)
#define LOGD(...)                                              \
    do {                                                       \
        if (::lib::log_debug()) ::lib::log_line(__VA_ARGS__); \
    } while (0)
