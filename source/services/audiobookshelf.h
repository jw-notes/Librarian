// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Audiobookshelf (https://www.audiobookshelf.org): signing in, libraries,
// books, play sessions, listening progress and bookmarks, covers and file
// downloads. Calls block: run them on a worker.
//
// Signing in uses the account in Settings (POST /login with x-return-tokens:
// an access token, renewed through /auth/refresh when it expires or is
// refused), or an API key from the server, which is used as it is.
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "audio/byte_source.h"
#include "audio/player.h"
#include "core/json.h"
#include "net/http.h"

namespace svc::abs {

struct Status {
    int status = 0;     // HTTP status, -1 when nothing answered
    std::string error;  // empty when it worked
    bool ok() const { return error.empty(); }
};

struct Library {
    std::string id, name, media_type;  // media_type: "book" or "podcast"
};

struct Chapter {
    double start = 0, end = 0;
    std::string title;
};

struct AudioFile {
    std::string ino, filename, mime, codec;
    std::string url;  // a file elsewhere (LibriVox on archive.org); empty for the server's
    uint64_t size = 0;
    double start = 0, duration = 0;  // its place in the book
};

struct Progress {
    bool found = false;
    double current_time = 0, duration = 0, progress = 0;
    bool finished = false;
    int64_t updated = 0;  // ms since 1970
};

struct Book {
    std::string id, library_id, title, subtitle, author, narrator, series, description, publisher, year;
    std::vector<std::string> genres;
    double duration = 0;
    int chapter_count = 0, file_count = 0;
    uint64_t size = 0;
    int64_t added = 0;
    Progress progress;
    std::string cover_url;  // a cover elsewhere (LibriVox); empty for the server's
    // From item() only.
    std::vector<Chapter> chapters;
    std::vector<AudioFile> files;
};

struct Bookmark {
    std::string item_id, title;
    double time = 0;
    int64_t created = 0;
};

struct Session {
    std::string id;
    int play_method = 0;  // 0 direct play, 2 transcoded (HLS)
    double current_time = 0, duration = 0;
    std::vector<audio::Track> tracks;
    std::vector<Chapter> chapters;
};

// The server address from Settings, normalised ("" if none).
std::string server();
bool configured();
// After the settings changed: forget tokens of the old server or account.
void settings_changed();

Status sign_in();
bool signed_in();
std::string user_name();
// Headers for requests to the server (for streaming and covers).
audio::HeaderFn auth_headers();

Status libraries(std::vector<Library>& out);
// sort: "title", "author", "added" (newest first) or "duration".
Status books(const std::string& library, int page, int per_page, const std::string& sort, std::vector<Book>& out,
             int& total);
Status search(const std::string& library, const std::string& query, std::vector<Book>& out);
Status in_progress(std::vector<Book>& out);
Status item(const std::string& id, Book& out);
// Everything of the user's: progress per book, and every bookmark.
Status me(std::map<std::string, Progress>& progress, std::vector<Bookmark>& bookmarks);

// Asks the server to play a book. Direct play for the formats Librarian
// decodes; anything else the server transcodes (HLS).
// `force_transcode`: ask for HLS even for formats Librarian plays itself (tests).
Status start_session(const std::string& id, Session& out, bool force_transcode = false);
Status sync_session(const std::string& session, double current_time, double listened, double duration);
Status close_session(const std::string& session, double current_time, double listened, double duration);

// Whether a progress update says the book is finished. The server marks a
// book finished by itself in its last 10 seconds, and an update saying "not
// finished" then resets its position to 0: so routine updates leave it alone.
enum class Finished { Leave, Yes, No };
Status set_progress(const std::string& id, double current_time, double duration, Finished finished = Finished::Leave);
Status add_bookmark(const std::string& id, double time, const std::string& title);
Status remove_bookmark(const std::string& id, double time);

// Downloads one of a book's audio files to `path` (via a .part file).
Status download_file(const std::string& id, const std::string& ino, const std::string& path, const lib::Progress& progress);

// Saves the book's cover as a JPEG (404 when it has none).
Status download_cover(const std::string& id, const std::string& path);

// Image-cache source of a book's cover ("abs-cover:<id>"), and its loader.
std::string cover_source(const std::string& id);
void register_cover_loader();

// For Settings: signs in and counts the libraries and books.
Status test_connection(std::string& summary);

// ---- parsing, exposed for the tests -------------------------------------------------------
Book parse_book(const lib::Json& item);
Progress parse_progress(const lib::Json& j);
bool parse_session(const lib::Json& j, const std::string& server, Session& out);
// Seconds since 1970 of a JWT's "exp" claim, 0 if it has none.
int64_t token_expiry(const std::string& jwt);
// Its lifetime in seconds (exp - iat), 0 if unknown: expiry is judged by the
// console's monotonic clock from when the token arrived, not by its date.
int64_t token_lifetime(const std::string& jwt);

}  // namespace svc::abs
