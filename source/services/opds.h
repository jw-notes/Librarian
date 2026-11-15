// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// OPDS 1.x catalogues (Atom feeds), as served by Calibre-Web at /opds:
// navigation feeds (lists of sections) and acquisition feeds (books).
#include <stdint.h>

#include <string>
#include <vector>

namespace svc {

struct OpdsAcquisition {
    std::string href;    // absolute
    std::string type;    // MIME type
    std::string format;  // "epub", "pdf", "cbz"...
    uint64_t size = 0;
};

struct OpdsEntry {
    std::string id, title, author, summary, updated, language, publisher, issued;
    std::vector<std::string> categories;
    std::string series;
    double series_index = 0;
    std::string cover, thumbnail;          // absolute URLs
    std::vector<OpdsAcquisition> formats;  // empty for navigation entries
    std::string link;                      // navigation entries: the feed they lead to
    bool navigation = false;
    std::string book_id;                   // Calibre-Web's numeric id, when known
};

struct OpdsFeed {
    std::string title;
    std::vector<OpdsEntry> entries;
    std::string next, previous, search;  // absolute URLs (search: OpenSearch description or template)
};

// `status` (optional) receives the HTTP status, -1 when nothing answered.
bool opds_fetch(const std::string& url, OpdsFeed& out, std::string& error, int* status = nullptr);
// Parses a feed body (public for tests). `base` resolves relative links.
bool opds_parse(const std::string& xml, const std::string& base, OpdsFeed& out);
// A feed's search link made into a URL template with "{searchTerms}": as it
// is, or read from the OpenSearch description it points to. False if none.
bool opds_search_template(const std::string& search_link, std::string& out);
// An OpenSearch description's template (Atom results preferred), resolved
// against `base`. Public for tests.
bool opds_parse_opensearch(const std::string& xml, const std::string& base, std::string& out);
// The template filled in with `query` (other optional parameters left out).
std::string opds_search_url(const std::string& search_template, const std::string& query);
// The format extension a MIME type stands for ("application/epub+zip" -> "epub").
std::string format_for_mime(const std::string& type, const std::string& href);

}  // namespace svc
