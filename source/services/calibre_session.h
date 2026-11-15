// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// A logged-in Calibre-Web session (cookie + CSRF), for actions the OPDS feed
// does not cover: signing in with an account and uploading a book. OPDS
// browsing itself uses HTTP Basic auth (see calibre.cpp); these are separate
// mechanisms in Calibre-Web.
#include <string>

namespace svc::calibre {

enum class LoginResult { Ok, WrongCredentials, RateLimited, NoServer, NotCalibre, NoUploadRight };

struct SessionStatus {
    LoginResult result = LoginResult::NoServer;
    std::string message;  // a sentence for the UI
    bool ok() const { return result == LoginResult::Ok; }
};

// Signs in with the account in Settings (or the given credentials) and keeps
// the session in memory. Safe to call again; it re-signs in if needed.
SessionStatus login(const std::string& user, const std::string& password);
// Signs in using Settings' stored Calibre-Web account.
SessionStatus login_from_settings();

// Uploads a local file as a new book. Signs in first if needed. `message` gets
// a sentence describing success or why it failed.
bool upload_book(const std::string& path, std::string& message);

// Clears the in-memory session (on sign-out or a credential change).
void forget_session();

}  // namespace svc::calibre
