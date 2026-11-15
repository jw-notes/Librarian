// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Kiwix Server (kiwix-serve 3.x / libkiwix 12+) client: the OPDS v2 catalogue,
// title suggestions, full-text search and article download.
#include <stdint.h>

#include <string>
#include <vector>

#include "doc/document.h"

namespace svc {

struct ZimBook {
    std::string id;       // catalogue UUID
    std::string name;     // e.g. wikipedia_en_all
    std::string content;  // the path segment under /content/, e.g. wikipedia_en_all_maxi_2024-01
    std::string title, summary, language, category, flavour, updated;
    int64_t articles = 0, media = 0;
    std::string icon;     // absolute URL of the 48px illustration
    // From the Kiwix online library: the .zim file and its size.
    std::string download;
    uint64_t size = 0;
    // The file name to save it as ("freecodecamp_en_all_2026-08.zim").
    std::string file_name() const;
    // Where it is read from: the Kiwix Server in Settings when empty, or
    // svc::kZimServer for a file on the console or a USB drive (`local`).
    std::string server;
    bool local = false, on_usb = false;
    std::string file;  // a local collection's file
};

struct SearchHit {
    std::string title;
    std::string url;      // absolute article URL
    std::string snippet;  // plain text
    std::string words;    // "9,993" as the server formats it
};

struct Article {
    doc::Document doc;
    std::string url;   // final address after redirects
    std::string path;  // "A/Volcano"
    std::string title;
};

namespace kiwix {

// The Kiwix online library (its catalogue; the files come from download.kiwix.org).
constexpr const char* kOnlineLibrary = "https://library.kiwix.org";

struct OnlineQuery {
    std::string text, language, category;  // empty: any
    int start = 0, count = 48;
};
// One page of the online library. `total`: how many match in all.
bool online_catalog(const OnlineQuery& query, std::vector<ZimBook>& out, int& total, std::string& error,
                    int* status = nullptr);
// The online library's languages (ISO 639-3 code, name, number of
// collections) and categories ("wikipedia", "gutenberg"...).
struct Facet {
    std::string code, title;
    int count = 0;
};
bool online_languages(std::vector<Facet>& out, std::string& error);
bool online_categories(std::vector<Facet>& out, std::string& error);
// Parsers, shared with the server's catalogue (`base`: the feed's address).
void parse_entries(const std::string& xml, const std::string& base, std::vector<ZimBook>& out, int* total = nullptr);
void parse_facets(const std::string& xml, bool languages, std::vector<Facet>& out);

// `status` (optional) receives the HTTP status, -1 when nothing answered.
bool catalog(const std::string& server, std::vector<ZimBook>& out, std::string& error, int* status = nullptr);
std::string main_page_url(const std::string& server, const ZimBook& book);
std::string random_url(const std::string& server, const std::string& content);
bool suggest(const std::string& server, const std::string& content, const std::string& term, int count,
             std::vector<SearchHit>& out, std::string& error);
// `start` is 0-based here.
bool search(const std::string& server, const std::string& content, const std::string& pattern, int start, int count,
            std::vector<SearchHit>& out, int& total, std::string& error);
bool fetch_article(const std::string& url, Article& out, std::string& error);
// Splits ".../content/<content>/<path>" into its parts.
bool split_article_url(const std::string& url, std::string& content, std::string& path);
// True for a page that is only a shell for JavaScript (some ZIMs are web
// apps: freeCodeCamp, zimit captures), which Librarian cannot show.
bool looks_like_script_app(const std::string& html, const doc::Document& converted);
// True for links that point at another article on the same server.
bool is_article_link(const std::string& server, const std::string& url);

}  // namespace kiwix
}  // namespace svc
