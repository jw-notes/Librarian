// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Collections (ZIM files) on the console and on USB drives, read without a
// Kiwix Server. They are served in-process at kZimServer the way kiwix-serve
// serves its collections (/content/<name>/<path>, /suggest, /random, the
// illustration), so the Knowledge screens and the article reader treat them
// like a server's. Full-text search is not available for them.
//
// Looked for in: "<data>/zim" (downloads to the console), and on each USB
// drive in its top folder, "Librarian", "Librarian/Kiwix" (downloads to the
// drive), "Kiwix" and "zim".
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "doc/zim.h"
#include "services/kiwix.h"

namespace svc {

constexpr const char* kZimServer = "http://zim.librarian";

class ZimShelf {
public:
    // Registers the in-process server. Once, at startup.
    void start();
    // Looks for ZIM files (on a worker; call from the UI thread). Opening a new
    // file reads its metadata; files seen before are not opened again.
    void scan_async();
    // The same, on the calling thread (tests, workers).
    void scan_now();
    bool scanning() const;
    std::vector<ZimBook> books() const;
    size_t count() const;
    // Counts each change of the list.
    uint64_t generation() const;
    // On the UI thread, every frame: rescans when the local library (which
    // finds ZIM files anywhere on the drives) has changed.
    void tick();
    // A ZIM file anywhere: added to the list (opened to read its metadata) and
    // returned. Call on a worker.
    bool adopt(const std::string& path, ZimBook& out, std::string& error);
    // Deletes a collection's file (and forgets it). Call on a worker.
    bool remove(const std::string& content, std::string& error);
    // An open archive for a collection (opened on first use, a few kept open).
    std::shared_ptr<zim::Archive> archive(const std::string& content);
};
ZimShelf& zim_shelf();

// The in-process server's answer to one request (exposed for tests).
void zim_serve(const std::string& url, int& status, std::string& type, std::string& location, std::string& body);

}  // namespace svc
