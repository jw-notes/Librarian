// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Cover pictures for local files, as image-cache sources "cover:<path>": the
// EPUB's cover image, a comic's first page, or a PDF's first page.
//
// Each cover found is also kept on disk, small, in <data>/covers/ (a JPEG of
// at most 600 x 900 named after the file's name and size, as the library's
// fingerprints are), so later launches show it without opening the book:
// from a USB drive read through GoldHEN's FTP server that would mean pulling
// much of the file through the server again.
#include "doc/covers.h"

#include "doc/zim.h"

#include <pthread.h>
#include <stdlib.h>

#include <algorithm>
#include <map>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/text.h"
#include "gfx/image.h"
#include "platform/platform.h"
#include "services/library.h"
#include "services/local_files.h"
#include "doc/archive.h"
#include "doc/epub.h"
#include "doc/mobi.h"
#include "doc/fb2.h"
#include "doc/office.h"
#include "doc/paged.h"
#include "gfx/image_cache.h"
#include "net/http.h"
#include "net/url.h"
#include "services/settings.h"

namespace doc {

std::string cover_key(const std::string& path) { return "cover:" + path; }

std::string relocate_cover(const std::string& source, const std::string& from, const std::string& to) {
    if (source == cover_key(from)) return to.empty() ? "" : cover_key(to);
    const std::string archive = archive_image_key(from, "");
    if (lib::starts_with(source, archive))
        return to.empty() ? "" : archive_image_key(to, source.substr(archive.size()));
    return source;
}

namespace {

constexpr int kCachedW = 600, kCachedH = 900, kCachedQuality = 85;
// More than this many covers kept: the folder is emptied and fills again with
// what is shown (no modification times on the console to find the oldest).
constexpr size_t kMaxCached = 3000;

std::string cache_dir() { return lib::join_path(platform::data_root(), "covers"); }

std::string cache_file(const std::string& path, uint64_t size) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : svc::file_fingerprint(path, size)) h = (h ^ c) * 1099511628211ull;
    char name[32];
    snprintf(name, sizeof name, "%016llx.jpg", (unsigned long long)h);
    return lib::join_path(cache_dir(), name);
}

void trim_cache() {
    std::vector<lib::DirEntry> entries;
    if (!lib::list_dir(cache_dir(), entries, false) || entries.size() <= kMaxCached) return;
    LOG("covers: %zu kept covers, starting the cache again", entries.size());
    for (const auto& e : entries)
        if (!e.is_dir) lib::remove_file(lib::join_path(cache_dir(), e.name));
}

bool extract_cover(const std::string& path, std::string& bytes) {
    const std::string ext = svc::format_of(path);
    if (ext == "epub") {
        Epub epub;
        std::string error;
        if (!epub.open(path, error) || epub.cover_path().empty()) return false;
        return epub.archive()->read(epub.cover_path(), bytes, 32ull * 1024 * 1024);
    }
    if (ext == "mobi" || ext == "azw3") {
        Mobi book;
        std::string error;
        return book.open(path, error) && book.cover(bytes);
    }
    if (ext == "fb2") {
        Fb2 book;
        std::string error;
        if (!book.open(path, error)) return false;
        auto image = book.images().find(book.cover_key());
        if (image == book.images().end()) return false;
        bytes = image->second;
        return true;
    }
    if (ext == "cbz" || ext == "cbr" || ext == "cb7" || ext == "zip" || ext == "rar" || ext == "7z" || ext == "pdf") {
        return paged_cover(path, bytes);
    }
    // DOCX / ODT: the preview picture the word processor saved, if any.
    if (ext == "docx" || ext == "odt") return office_cover(path, bytes);
    if (ext == "zim") {
        std::string error;
        auto zim = zim::Archive::open(path, error);
        return zim && zim->illustration(bytes);
    }
    return false;
}

// A picture lying beside the book: one with the same name ("Emma.epub" and
// "Emma.jpg"), or, for a talking book that is a folder of its own (DAISY),
// the folder's cover.jpg / folder.jpg / front.jpg.
bool sidecar_cover(const std::string& path, std::string& bytes) {
    const std::string dir = lib::parent_dir(path), stem = lib::lower(lib::file_stem(path));
    const bool folder_book = svc::format_of(path) == "daisy";
    std::vector<lib::DirEntry> entries;
    if (!lib::list_dir(dir, entries, false)) return false;
    std::string best;
    for (const auto& e : entries) {
        if (e.is_dir) continue;
        const std::string ext = lib::file_extension(e.name);
        if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "webp") continue;
        const std::string s = lib::lower(lib::file_stem(e.name));
        if (s == stem) {
            best = e.name;
            break;
        }
        if (folder_book && (s == "cover" || s == "folder" || s == "front") && best.empty()) best = e.name;
    }
    return !best.empty() && lib::read_file(lib::join_path(dir, best), bytes, 16ull << 20) && !bytes.empty();
}

pthread_mutex_t g_hint_lock = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, std::pair<std::string, std::string>> g_hints;

bool cover_hint(const std::string& path, std::string& title, std::string& author) {
    pthread_mutex_lock(&g_hint_lock);
    auto it = g_hints.find(path);
    const bool found = it != g_hints.end();
    if (found) {
        title = it->second.first;
        author = it->second.second;
    }
    pthread_mutex_unlock(&g_hint_lock);
    return found;
}

// Lower-case words of letters and digits: "The Time Machine!" -> "the time machine".
std::string normalized(const std::string& s) {
    std::string out;
    bool space = true;
    for (unsigned char c : lib::lower(s)) {
        if (isalnum(c) || c >= 0x80) {
            out += (char)c;
            space = false;
        } else if (!space) {
            out += ' ';
            space = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Open Library (and, on the host, a local stand-in).
std::string search_base() {
#ifdef LIB_HOST
    if (const char* e = getenv("LIBRARIAN_OPENLIBRARY")) return e;
#endif
    return "https://openlibrary.org";
}
std::string covers_base() {
#ifdef LIB_HOST
    if (const char* e = getenv("LIBRARIAN_OPENLIBRARY")) return e;
#endif
    return "https://covers.openlibrary.org";
}

}  // namespace

bool cached_cover(const std::string& path, std::string& bytes) {
    const uint64_t size = lib::file_size(path);
    const std::string kept = size ? cache_file(path, size) : std::string();
    if (!kept.empty() && lib::read_file(kept, bytes, 8ull * 1024 * 1024) && !bytes.empty()) return true;
    // The book's own cover; else a picture beside it; else one looked up.
    if (!extract_cover(path, bytes) && !sidecar_cover(path, bytes)) {
        std::string title, author;
        if (!cover_hint(path, title, author) || !online_cover(title, author, bytes)) return false;
    }
    if (kept.empty()) return true;
    // Kept small: decoded once more here, scaled, and written as a JPEG.
    gfx::Image image;
    std::string jpeg;
    if (gfx::decode_image(bytes, image, kCachedW, kCachedH) && gfx::encode_jpeg(image, kCachedQuality, jpeg)) {
        lib::make_dirs(cache_dir());
        if (!lib::write_file_atomic(kept, jpeg)) LOG("covers: could not keep the cover of %s", path.c_str());
        bytes = std::move(jpeg);
    }
    return true;
}

void set_cover_hint(const std::string& path, const std::string& title, const std::string& author) {
    if (title.empty() || author.empty()) return;
    pthread_mutex_lock(&g_hint_lock);
    g_hints[path] = {title, author};
    pthread_mutex_unlock(&g_hint_lock);
}

std::string find_cover_key(const std::string& title, const std::string& author) {
    return title.empty() ? std::string() : "find-cover:" + title + "\x01" + author;
}

bool same_title(const std::string& ours, const std::string& theirs) {
    const std::string a = normalized(ours), b = normalized(theirs);
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    // One is the other with a subtitle or series added ("Emma" / "Emma: a novel").
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    return shorter.size() >= 4 && lib::starts_with(longer, shorter + " ");
}

bool online_cover(const std::string& title, const std::string& author, std::string& bytes) {
    if (title.empty()) return false;
    // Kept per title and author, found or not, so each is asked about once.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : "find|" + normalized(title) + "|" + normalized(author)) h = (h ^ c) * 1099511628211ull;
    char name[40];
    snprintf(name, sizeof name, "find-%016llx", (unsigned long long)h);
    const std::string kept = lib::join_path(cache_dir(), std::string(name) + ".jpg");
    const std::string none = lib::join_path(cache_dir(), std::string(name) + ".none");
    if (lib::read_file(kept, bytes, 8ull << 20) && !bytes.empty()) return true;
    if (!svc::settings().online_covers || lib::file_exists(none)) return false;

    lib::HttpRequest search;
    search.url = search_base() + "/search.json?title=" + lib::url_encode(title) +
                 (author.empty() ? std::string() : "&author=" + lib::url_encode(author)) + "&limit=5&fields=title,cover_i";
    search.timeout_seconds = 20;
    const lib::HttpResponse r = lib::http_request(search);
    if (!r.ok()) {
        // Not reachable now (offline, no HTTPS): asked again another time.
        LOG("covers: looking up \"%s\" failed: %s", title.c_str(), r.error.c_str());
        return false;
    }
    int64_t cover_id = 0;
    const lib::Json found = lib::Json::parse(r.body);
    for (const lib::Json& d : found["docs"].items())
        if (d["cover_i"].integer(0) > 0 && same_title(title, d["title"].str())) {
            cover_id = d["cover_i"].integer(0);
            break;
        }
    lib::make_dirs(cache_dir());
    if (!cover_id) {
        LOG("covers: no cover found for \"%s\"", title.c_str());
        lib::write_file_atomic(none, "");
        return false;
    }
    lib::HttpRequest get;
    get.url = covers_base() + "/b/id/" + std::to_string(cover_id) + "-L.jpg";
    get.timeout_seconds = 30;
    get.max_bytes = 8ull << 20;
    lib::HttpResponse img = lib::http_request(get);
    gfx::Image image;
    std::string jpeg;
    if (!img.ok() || !gfx::decode_image(img.body, image, kCachedW, kCachedH) || !gfx::encode_jpeg(image, kCachedQuality, jpeg)) {
        LOG("covers: the cover of \"%s\" could not be fetched", title.c_str());
        return false;
    }
    lib::write_file_atomic(kept, jpeg);
    LOG("covers: found a cover for \"%s\" on Open Library", title.c_str());
    bytes = std::move(jpeg);
    return true;
}

void register_cover_loader() {
    trim_cache();
    gfx::image_register_prefix_loader("cover:", [](const std::string& source, std::string& bytes) {
        return cached_cover(source.substr(6), bytes);
    });
    gfx::image_register_prefix_loader("find-cover:", [](const std::string& source, std::string& bytes) {
        const std::string rest = source.substr(11);
        const size_t sep = rest.find('\x01');
        return online_cover(rest.substr(0, sep), sep == std::string::npos ? "" : rest.substr(sep + 1), bytes);
    });
}

}  // namespace doc
