// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Turns a failed connection into a short explanation and concrete steps, for
// Settings' connection tests and for the Knowledge and Books tabs.
#include <string>
#include <vector>

namespace ui {

enum class Service { Kiwix, Calibre, Audiobookshelf };

enum class Problem { None, NoAnswer, Certificate, SignInRequired, SignInRejected, NoPermission, NotFound, WrongService,
                     ServerError, RateLimited, Other };

struct Diagnosis {
    Problem problem = Problem::None;
    std::string title;               // "Calibre-Web asked for a sign-in"
    std::vector<std::string> steps;  // what to check, in order
};

// `status`: HTTP status, -1 when nothing answered. `error`: the message the
// request produced (a 200 with an error means the reply was not what was expected).
Diagnosis diagnose(Service service, int status, const std::string& error);

// Opens the troubleshooting panel over the current screen; ✕ goes to Settings.
void open_troubleshooter(Service service, int status, const std::string& error);

}  // namespace ui
