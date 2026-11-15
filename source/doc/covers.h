// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <string>

namespace doc {

// The image-cache source for a local file's cover.
std::string cover_key(const std::string& path);
void register_cover_loader();

// Missing covers. A book with no cover of its own gets a picture lying beside
// it, or else (Settings › System › Find missing covers) one from Open
// Library, looked up by title and author.
//
// What the local library knows about a file, for that lookup (set on the UI
// thread when its metadata is read; only files that name a title and author).
void set_cover_hint(const std::string& path, const std::string& title, const std::string& author);
// The image-cache source of a cover looked up by title and author alone
// ("find-cover:..."): LibriVox books with none, audiobook folders without a
// picture. "" without a title.
std::string find_cover_key(const std::string& title, const std::string& author);
// The lookup itself (on a worker): a JPEG kept on the console, found before
// or looked up now. False when there is none (also remembered).
bool online_cover(const std::string& title, const std::string& author, std::string& bytes);
// Exposed for the tests: whether a search result's title names the same book.
bool same_title(const std::string& ours, const std::string& theirs);
// A local file's cover, from the cover cache or else from the file (then kept).
bool cached_cover(const std::string& path, std::string& bytes);
// A stored cover source for the file at `from`, now that it is at `to`: a
// "cover:" or EPUB "archive:" source is moved to the new path, and dropped
// when `to` is empty (the file cannot be reached). Others (URLs) are kept.
std::string relocate_cover(const std::string& source, const std::string& from, const std::string& to);

}  // namespace doc
