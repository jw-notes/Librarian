// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/librivox.h"

#include <stdlib.h>

#include <algorithm>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "net/http.h"
#include "net/url.h"
#include "services/listening.h"

namespace svc::librivox {

namespace {

constexpr const char* kApi = "https://librivox.org/api/feed/audiobooks/?format=json";
// The fields of a list (no chapters: a book can have a hundred or more).
constexpr const char* kListFields = "&fields=%7Bid,title,description,authors,totaltimesecs,url_iarchive,language,num_sections%7D";

std::string get(const std::string& url, std::string& error) {
    lib::HttpRequest r;
    r.url = url;
    r.timeout_seconds = 30;
    r.max_bytes = 16ull * 1024 * 1024;
    const lib::HttpResponse resp = lib::http_request(r);
    // The API answers 404 with {"error": "Audiobooks could not be found"} for no match.
    if (resp.status == 404) return "{\"books\":[]}";
    if (!resp.ok()) {
        error = lib::describe_failure(resp);
        return std::string();
    }
    return resp.body;
}

std::string plain(const std::string& html) {
    // Descriptions are HTML; the book page shows text.
    std::string text = lib::parse_markup(html, lib::MarkupMode::Html)->inner_text();
    return lib::collapse_space(lib::decode_entities(text));
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}

std::string person(const lib::Json& a) {
    return lib::trim(lib::trim(a["first_name"].str()) + " " + lib::trim(a["last_name"].str()));
}

}  // namespace

bool is_librivox(const std::string& id) { return lib::starts_with(id, kIdPrefix); }

bool parse_books(const std::string& json, std::vector<abs::Book>& out) {
    out.clear();
    bool ok = false;
    const lib::Json j = lib::Json::parse(json, &ok);
    if (!ok) return false;
    for (const lib::Json& b : j["books"].items()) {
        abs::Book book;
        book.id = kIdPrefix + b["id"].str();
        if (book.id == kIdPrefix) continue;
        book.title = lib::decode_entities(b["title"].str());
        std::vector<std::string> authors;
        for (const lib::Json& a : b["authors"].items()) authors.push_back(person(a));
        book.author = join(authors, ", ");
        book.description = plain(b["description"].str());
        book.duration = (double)b["totaltimesecs"].integer(0);
        book.publisher = "LibriVox";
        if (!b["language"].str().empty()) book.genres.push_back(b["language"].str());
        // The archive.org item gives the cover ("services/img/<identifier>").
        const std::string ia = b["url_iarchive"].str();
        const size_t slash = ia.find("/details/");
        if (slash != std::string::npos) book.cover_url = "https://archive.org/services/img/" + ia.substr(slash + 9);
        book.chapter_count = atoi(b["num_sections"].str().c_str());
        // Chapters (with extended=1): one MP3 each.
        std::vector<std::string> readers;
        double at = 0;
        for (const lib::Json& s : b["sections"].items()) {
            const std::string url = s["listen_url"].str();
            if (url.empty()) continue;
            abs::AudioFile f;
            f.url = url;
            f.ino = s["id"].str();
            f.mime = "audio/mpeg";
            f.codec = "mp3";
            f.duration = (double)atof(s["playtime"].str().c_str());
            if (f.duration <= 0) f.duration = (double)s["playtime"].integer(0);
            f.start = at;
            const int n = atoi(s["section_number"].str().c_str());
            char number[16];
            snprintf(number, sizeof number, "%03d", n > 0 ? n : (int)book.files.size() + 1);
            const std::string title = lib::trim(lib::decode_entities(s["title"].str()));
            f.filename = lib::safe_file_name(std::string(number) + " " + (title.empty() ? "Part" : title)) + ".mp3";
            book.chapters.push_back({at, at + f.duration, title.empty() ? "Part " + std::to_string(n) : title});
            at += f.duration;
            book.files.push_back(f);
            for (const lib::Json& r : s["readers"].items()) {
                const std::string name = lib::trim(r["display_name"].str());
                if (!name.empty() && std::find(readers.begin(), readers.end(), name) == readers.end()) readers.push_back(name);
            }
        }
        if (!book.files.empty()) {
            book.file_count = (int)book.files.size();
            book.chapter_count = (int)book.chapters.size();
            if (at > 0) book.duration = at;
        }
        if (readers.size() > 3) {
            readers.resize(3);
            readers.push_back("others");
        }
        book.narrator = join(readers, ", ");
        out.push_back(std::move(book));
    }
    return true;
}

bool recent(std::vector<abs::Book>& out, std::string& error) {
    // The last 120 days by the console's clock; the newest first.
    const long long since = (long long)(epoch_ms() / 1000) - 120ll * 86400;
    const std::string body = get(std::string(kApi) + kListFields + "&limit=100&since=" + std::to_string(since), error);
    if (body.empty() || !parse_books(body, out)) {
        if (error.empty()) error = "LibriVox's answer could not be read.";
        return false;
    }
    std::sort(out.begin(), out.end(), [](const abs::Book& a, const abs::Book& b) {
        return atoll(a.id.c_str() + 9) > atoll(b.id.c_str() + 9);
    });
    return true;
}

bool search(const std::string& text, std::vector<abs::Book>& out, std::string& error) {
    out.clear();
    const std::string q = lib::url_encode("^" + lib::trim(text));
    std::vector<abs::Book> titles, authors;
    const std::string a = get(std::string(kApi) + kListFields + "&limit=50&title=" + q, error);
    if (a.empty() || !parse_books(a, titles)) return false;
    // Authors are searched by last name.
    const std::string b = get(std::string(kApi) + kListFields + "&limit=50&author=" + q, error);
    if (!b.empty()) parse_books(b, authors);
    error.clear();
    out = std::move(titles);
    for (auto& book : authors) {
        bool dup = false;
        for (const auto& o : out) dup = dup || o.id == book.id;
        if (!dup) out.push_back(std::move(book));
    }
    return true;
}

bool book(const std::string& id, abs::Book& out, std::string& error) {
    if (!is_librivox(id)) return false;
    const std::string body = get(std::string(kApi) + "&extended=1&id=" + lib::url_encode(id.substr(9)), error);
    std::vector<abs::Book> books;
    if (body.empty() || !parse_books(body, books) || books.empty()) {
        if (error.empty()) error = "LibriVox does not list this book any more.";
        return false;
    }
    out = std::move(books[0]);
    if (out.files.empty()) {
        error = "LibriVox lists no recordings for this book yet.";
        return false;
    }
    LOG("librivox: %s, %zu chapters", out.title.c_str(), out.files.size());
    return true;
}

}  // namespace svc::librivox
