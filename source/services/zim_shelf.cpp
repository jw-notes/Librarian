// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/zim_shelf.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <map>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/text.h"
#include "core/worker.h"
#include "net/http.h"
#include "net/url.h"
#include "platform/platform.h"
#include "services/local_library.h"
#include "services/zim_downloads.h"

namespace svc {

namespace {

struct Shelf {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    std::vector<ZimBook> books;
    // What each file said about itself, by name + size (not opened again).
    std::map<std::string, ZimBook> known;
    std::list<std::pair<std::string, std::shared_ptr<zim::Archive>>> open;  // content -> archive, recent first
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> scanning{false};
    // ZIM files found elsewhere: by the local library (anywhere on the drives
    // and /data/books) or opened from the Local section. Path and size.
    std::vector<std::pair<std::string, uint64_t>> found, adopted;
    uint64_t library_generation = ~0ull;
};

Shelf& shelf() {
    static Shelf s;
    return s;
}

struct Locked {
    explicit Locked(pthread_mutex_t* m) : m_(m) { pthread_mutex_lock(m_); }
    ~Locked() { pthread_mutex_unlock(m_); }
    pthread_mutex_t* m_;
};

constexpr size_t kOpenArchives = 4;

std::vector<std::string> folders() {
    std::vector<std::string> out = {zim_folder(false)};
    for (const std::string& slot : platform::usb_mount_points()) {
        std::vector<lib::DirEntry> entries;
        if (!lib::list_dir(slot, entries, false) || entries.empty()) continue;  // no drive
        for (const char* sub : {"", "/Librarian", "/Librarian/Kiwix", "/Kiwix", "/zim", "/ZIM"})
            out.push_back(slot + sub);
    }
    return out;
}

bool under_usb(const std::string& path) {
    for (const std::string& slot : platform::usb_mount_points())
        if (lib::starts_with(path, slot + "/")) return true;
    return false;
}

// Reads what a file says about itself.
bool describe(const std::string& path, uint64_t size, ZimBook& b) {
    std::string error;
    auto a = zim::Archive::open(path, error);
    if (!a) {
        LOG("zim: %s not read: %s", lib::file_name(path).c_str(), error.c_str());
        return false;
    }
    b.title = a->metadata("Title");
    if (b.title.empty()) b.title = lib::file_stem(path);
    b.summary = a->metadata("Description");
    b.language = a->metadata("Language");
    b.name = a->metadata("Name");
    b.flavour = a->metadata("Flavour");
    b.updated = a->metadata("Date").substr(0, 10);
    b.articles = a->article_count();
    if (!b.articles) b.articles = a->entry_count();
    std::string png;
    if (a->illustration(png)) b.icon = "y";  // made into an address once the name is known
    b.size = size;
    b.file = path;
    LOG("zim: %s: \"%s\", %lld articles, ZIM %s layout", lib::file_name(path).c_str(), b.title.c_str(),
        (long long)b.articles, a->new_namespace() ? "new" : "old");
    return true;
}

std::string query_param(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return std::string();
    for (const std::string& pair : lib::split(path.substr(q + 1), '&')) {
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, key) == 0) {
            std::string v = pair.substr(eq + 1);
            std::replace(v.begin(), v.end(), '+', ' ');
            return lib::url_decode(v);
        }
    }
    return std::string();
}

// The address of an entry, as kiwix-serve gives it: old-layout files keep the
// namespace in the path ("A/Volcano", "I/m/picture.jpg").
std::string entry_url(const std::string& content, const zim::Archive& a, const zim::Entry& e) {
    const std::string path = a.new_namespace() ? e.path : std::string(1, e.ns) + "/" + e.path;
    return std::string(kZimServer) + "/content/" + content + "/" + lib::url_encode(path, true);
}

}  // namespace

void zim_serve(const std::string& url, int& status, std::string& type, std::string& location, std::string& body) {
    status = 404;
    type = "text/plain";
    body.clear();
    lib::Url u;
    if (!lib::parse_url(url, u)) return;
    const std::string full = u.path;
    const std::string path = full.substr(0, full.find('?'));
    auto redirect = [&](const std::string& to) {
        status = 302;
        location = to;
    };
    if (lib::starts_with(path, "/content/")) {
        const std::string rest = path.substr(9);
        const size_t slash = rest.find('/');
        const std::string content = lib::url_decode(rest.substr(0, slash));
        auto a = zim_shelf().archive(content);
        if (!a) return;
        const std::string inner = slash == std::string::npos ? std::string() : lib::url_decode(rest.substr(slash + 1));
        zim::Entry e;
        if (inner.empty()) {
            if (a->main_page(e) && a->resolve(e)) redirect(entry_url(content, *a, e));
            return;
        }
        bool found;
        if (a->new_namespace()) {
            found = a->find('C', inner, e);
        } else {
            found = inner.size() > 2 && inner[1] == '/' && a->find(inner[0], inner.substr(2), e);
            if (!found) found = a->find('A', inner, e);  // a link without its namespace
        }
        if (!found) return;
        if (e.is_redirect()) {
            zim::Entry target = e;
            if (a->resolve(target)) redirect(entry_url(content, *a, target));
            return;
        }
        if (!a->read(e, body, type)) {
            status = 500;
            return;
        }
        status = 200;
        return;
    }
    if (path == "/suggest") {
        const std::string content = query_param(full, "content"), term = query_param(full, "term");
        const int count = std::max(1, std::min(50, atoi(query_param(full, "count").c_str())));
        auto a = zim_shelf().archive(content);
        if (!a) return;
        std::vector<zim::Entry> hits;
        a->suggest(term, (size_t)count, hits);
        lib::Json list = lib::Json::array();
        for (const auto& h : hits) {
            lib::Json item = lib::Json::object();
            item.set("value", h.title);
            item.set("label", h.title);
            item.set("kind", "path");
            item.set("path", a->new_namespace() ? h.path : std::string(1, h.ns) + "/" + h.path);
            list.push(std::move(item));
        }
        status = 200;
        type = "application/json";
        body = list.dump();
        return;
    }
    if (path == "/random") {
        const std::string content = query_param(full, "content");
        auto a = zim_shelf().archive(content);
        zim::Entry e;
        if (a && a->random_article(e)) redirect(entry_url(content, *a, e));
        return;
    }
    if (lib::starts_with(path, "/catalog/v2/illustration/")) {
        std::string content = path.substr(25);
        while (!content.empty() && content.back() == '/') content.pop_back();
        auto a = zim_shelf().archive(lib::url_decode(content));
        if (a && a->illustration(body)) {
            status = 200;
            type = "image/png";
        }
        return;
    }
    // /search (the full-text index) and anything else: not here.
}

void ZimShelf::start() {
    lib::http_register_local(kZimServer, [](const std::string& url, lib::HttpRawResult& head, std::string& body) {
        zim_serve(url, head.status, head.content_type, head.location, body);
    });
}

void ZimShelf::scan_now() {
    Shelf& s = shelf();
    std::vector<std::pair<std::string, uint64_t>> files;
    for (const std::string& dir : folders()) {
        std::vector<lib::DirEntry> entries;
        if (!lib::list_dir(dir, entries)) continue;
        for (const auto& e : entries) {
            if (e.is_dir || lib::file_extension(e.name) != "zim") continue;
            const std::string path = lib::join_path(dir, e.name);
            bool dup = false;
            for (const auto& f : files) dup = dup || f.first == path;
            if (!dup) files.push_back({path, e.size});
        }
    }
    {
        Locked lock(&s.lock);
        // Opened from Local: kept while the file is there.
        s.adopted.erase(std::remove_if(s.adopted.begin(), s.adopted.end(),
                                       [](const auto& x) { return !lib::file_exists(x.first); }),
                        s.adopted.end());
        for (const auto* list : {&s.found, &s.adopted})
            for (const auto& x : *list) {
                bool dup = false;
                for (const auto& f : files) dup = dup || f.first == x.first;
                if (!dup) files.push_back(x);
            }
    }
    std::vector<ZimBook> found;
    for (const auto& [path, size] : files) {
        const std::string key = lib::file_name(path) + "|" + std::to_string(size);
        ZimBook b;
        bool known;
        {
            Locked lock(&s.lock);
            auto it = s.known.find(key);
            known = it != s.known.end();
            if (known) b = it->second;
        }
        if (!known) {
            if (!describe(path, size, b)) continue;
            Locked lock(&s.lock);
            s.known[key] = b;
        }
        b.file = path;
        b.on_usb = under_usb(path);
        found.push_back(b);
    }
    // Names (the address segment): the file's name, made unique.
    for (size_t i = 0; i < found.size(); ++i) {
        ZimBook& b = found[i];
        std::string name = lib::file_stem(b.file);
        for (int n = 2;; ++n) {
            bool taken = false;
            for (size_t j = 0; j < i; ++j) taken = taken || found[j].content == name;
            if (!taken) break;
            name = lib::file_stem(b.file) + "~" + std::to_string(n);
        }
        b.content = name;
        b.server = kZimServer;
        b.local = true;
        b.id = "local:" + name;
        b.icon = b.icon.empty() ? std::string() : std::string(kZimServer) + "/catalog/v2/illustration/" + lib::url_encode(name) + "/";
    }
    std::sort(found.begin(), found.end(), [](const ZimBook& a, const ZimBook& b) { return lib::natural_less(a.title, b.title); });
    bool changed;
    {
        Locked lock(&s.lock);
        changed = found.size() != s.books.size();
        for (size_t i = 0; !changed && i < found.size(); ++i)
            changed = found[i].file != s.books[i].file || found[i].content != s.books[i].content;
        s.books = std::move(found);
        // Archives of files no longer listed are closed.
        s.open.remove_if([&](const auto& entry) {
            for (const auto& b : s.books)
                if (b.content == entry.first && b.file == entry.second->path()) return false;
            return true;
        });
    }
    if (changed) {
        ++s.generation;
        LOG("zim: %zu collection(s) on the console and USB drives", count());
    }
}

void ZimShelf::scan_async() {
    Shelf& s = shelf();
    // What the local library has found (UI thread only, so read here).
    std::vector<std::pair<std::string, uint64_t>> found;
    for (const LocalDoc& d : local_library().docs())
        if (d.format == "zim" && d.available) found.push_back({d.path, d.size});
    {
        Locked lock(&s.lock);
        s.found = std::move(found);
        s.library_generation = local_library().generation();
    }
    if (s.scanning.exchange(true)) return;
    lib::run_async([this] {
        scan_now();
        lib::post([] {
            shelf().scanning = false;
            platform::request_redraw();
        });
    });
}

bool ZimShelf::scanning() const { return shelf().scanning; }

void ZimShelf::tick() {
    if (local_library().generation() != shelf().library_generation && !shelf().scanning) scan_async();
}

bool ZimShelf::adopt(const std::string& path, ZimBook& out, std::string& error) {
    Shelf& s = shelf();
    for (const auto& b : books())
        if (b.file == path) {
            out = b;
            return true;
        }
    const uint64_t size = lib::file_size(path);
    {
        Locked lock(&s.lock);
        s.adopted.push_back({path, size});
    }
    scan_now();
    for (const auto& b : books())
        if (b.file == path) {
            out = b;
            return true;
        }
    error = "This ZIM file could not be read (see the log).";
    return false;
}

std::vector<ZimBook> ZimShelf::books() const {
    Locked lock(&shelf().lock);
    return shelf().books;
}

size_t ZimShelf::count() const {
    Locked lock(&shelf().lock);
    return shelf().books.size();
}

uint64_t ZimShelf::generation() const { return shelf().generation; }

std::shared_ptr<zim::Archive> ZimShelf::archive(const std::string& content) {
    Shelf& s = shelf();
    std::string file;
    {
        Locked lock(&s.lock);
        for (auto it = s.open.begin(); it != s.open.end(); ++it)
            if (it->first == content) {
                s.open.splice(s.open.begin(), s.open, it);
                return it->second;
            }
        for (const auto& b : s.books)
            if (b.content == content) file = b.file;
    }
    if (file.empty()) return nullptr;
    std::string error;
    auto a = zim::Archive::open(file, error);
    if (!a) {
        LOG("zim: %s not opened: %s", lib::file_name(file).c_str(), error.c_str());
        return nullptr;
    }
    Locked lock(&s.lock);
    for (const auto& entry : s.open)
        if (entry.first == content) return entry.second;  // another thread was quicker
    s.open.push_front({content, a});
    while (s.open.size() > kOpenArchives) s.open.pop_back();
    return a;
}

bool ZimShelf::remove(const std::string& content, std::string& error) {
    Shelf& s = shelf();
    std::string file;
    {
        Locked lock(&s.lock);
        for (const auto& b : s.books)
            if (b.content == content) file = b.file;
        // Closed first: the file is still open otherwise.
        s.open.remove_if([&](const auto& entry) { return entry.first == content; });
    }
    if (file.empty()) {
        error = "That collection is no longer there.";
        return false;
    }
    if (!lib::remove_file(file)) {
        error = "The file could not be deleted.";
        return false;
    }
    LOG("zim: deleted %s", file.c_str());
    {
        Locked lock(&s.lock);
        for (auto* list : {&s.found, &s.adopted})
            list->erase(std::remove_if(list->begin(), list->end(), [&](const auto& x) { return x.first == file; }),
                        list->end());
        s.books.erase(std::remove_if(s.books.begin(), s.books.end(), [&](const ZimBook& b) { return b.file == file; }),
                      s.books.end());
    }
    ++s.generation;
    return true;
}

ZimShelf& zim_shelf() {
    static ZimShelf z;
    return z;
}

}  // namespace svc
