// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/local_library.h"

#include "doc/zim.h"

#include <algorithm>
#include <functional>
#include <memory>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "core/worker.h"
#include "doc/archive.h"
#include "doc/covers.h"
#include "doc/daisy.h"
#include "doc/epub.h"
#include "doc/fb2.h"
#include "doc/mobi.h"
#include "doc/office.h"
#include "doc/paged.h"
#include "gfx/image_cache.h"
#include "platform/platform.h"
#include "services/library.h"

namespace svc {

namespace {

constexpr int kMaxDepth = 12;
constexpr size_t kMaxDocuments = 20000, kMaxDirectories = 20000;
constexpr uint64_t kPollMs = 3000;

std::string index_path() { return lib::join_path(platform::data_root(), "local_index.json"); }

bool skipped_folder(const std::string& name) {
    return name.empty() || name[0] == '.' || name[0] == '$' || name == "System Volume Information" || name == "RECYCLER" ||
           name == "LOST.DIR";
}

// A USB slot has a drive in it when its folder lists anything.
bool slot_has_drive(const std::string& path) {
    std::vector<lib::DirEntry> entries;
    return lib::list_dir(path, entries, false) && !entries.empty();
}

// FB2 metadata without reading the whole book (they carry their pictures inline).
void fb2_metadata(LocalDoc& d) {
    std::string head;
    if (lib::ends_with(lib::lower(d.path), ".zip") || lib::file_extension(d.path) == "fbz") {
        doc::Fb2 book;
        std::string error;
        if (d.size < 32ull * 1024 * 1024 && book.open(d.path, error)) {
            d.title = book.title();
            d.author = book.author();
        }
        return;
    }
    FILE* f = lib::open_file(d.path);
    if (!f) return;
    head.resize(256 * 1024);
    head.resize(fread(&head[0], 1, head.size(), f));
    fclose(f);
    const size_t end = head.find("</description>");
    if (end == std::string::npos) return;
    std::string encoding;
    const size_t decl = head.find("encoding=");
    if (decl != std::string::npos && decl < 200 && decl + 10 < head.size()) {
        const char quote = head[decl + 9];
        const size_t close = head.find(quote, decl + 10);
        if (close != std::string::npos) encoding = head.substr(decl + 10, close - decl - 10);
    }
    auto root = lib::parse_markup(doc::decode_legacy(head.substr(0, end + 14), encoding), lib::MarkupMode::Xml);
    const lib::Node* info = root->find("title-info");
    if (!info) return;
    d.title = info->child_text("book-title");
    for (const lib::Node* a : info->elements("author")) {
        std::string name = lib::collapse_space(a->child_text("first-name") + " " + a->child_text("last-name"));
        if (name.empty()) name = a->child_text("nickname");
        if (!name.empty()) d.author += (d.author.empty() ? "" : ", ") + name;
    }
}

}  // namespace

std::string LocalDoc::place() const {
    if (origin == Origin::Downloaded) return "Downloaded";
    if (origin == Origin::Usb) return "USB " + std::to_string(slot + 1);
    return "Internal";
}

std::vector<ScanRoot> present_roots() {
    std::vector<ScanRoot> roots = {{downloads_dir(), Origin::Downloaded, -1}};
    for (const std::string& dir : platform::internal_document_dirs())
        if (lib::dir_exists(dir)) roots.push_back({dir, Origin::Internal, -1});
    const auto slots = platform::usb_mount_points();
    for (size_t i = 0; i < slots.size(); ++i)
        if (slot_has_drive(slots[i])) roots.push_back({slots[i], Origin::Usb, (int)i});
    return roots;
}

std::vector<LocalDoc> scan_root(const ScanRoot& root, const std::map<std::string, LocalDoc>& cache, ScanStats* stats) {
    ScanStats local;
    ScanStats& st = stats ? *stats : local;
    std::vector<LocalDoc> out;
    std::function<void(const std::string&, const std::string&, int)> walk = [&](const std::string& dir,
                                                                                  const std::string& rel, int depth) {
        if (out.size() >= kMaxDocuments || st.directories >= kMaxDirectories) {
            st.truncated = true;
            return;
        }
        ++st.directories;
        std::vector<lib::DirEntry> entries;
        // Sizes are only needed for documents: measuring every file in a folder
        // of game packages would mean opening each one.
        if (!lib::list_dir(dir, entries, false)) return;
        // A DAISY 2.02 book's folder: its ncc.html is the book; the text files
        // beside it are parts of it, not documents of their own.
        bool daisy = false;
        for (const lib::DirEntry& e : entries) daisy = daisy || lib::iequals(e.name, "ncc.html");
        for (const lib::DirEntry& e : entries) {
            if (e.is_dir) {
                if (!skipped_folder(e.name) && depth < kMaxDepth)
                    walk(lib::join_path(dir, e.name), rel.empty() ? e.name : rel + "/" + e.name, depth + 1);
                continue;
            }
            if (e.name[0] == '.' || lib::ends_with(e.name, ".part") || lib::ends_with(e.name, ".tmp")) continue;
            const std::string format = format_of(e.name);
            // A ZIP file is listed only when it holds a DAISY talking book
            // (looked into once; the index remembers).
            bool daisy_zip = false;
            if (format == "zip") {
                const std::string path = lib::join_path(dir, e.name);
                auto cached = cache.find(path);
                std::string t, a;
                daisy_zip = cached != cache.end() ? cached->second.format == "daisy" : doc::daisy_metadata(path, t, a);
                if (!daisy_zip) continue;
            } else if (!readable_extension(format)) {
                continue;
            }
            if (daisy && (format == "html" || format == "htm" || format == "xhtml")) continue;
            LocalDoc d;
            d.path = lib::join_path(dir, e.name);
            d.format = daisy_zip ? "daisy" : format == "azw" || format == "prc" ? "mobi" : format;
            d.kind = kind_of(d.format);
            d.origin = root.origin;
            d.slot = root.slot;
            d.folder = rel;
            d.inode = e.inode;
            // Unchanged since the last scan (same path and file serial number):
            // keep its size and metadata.
            auto cached = cache.find(d.path);
            if (cached != cache.end() && e.inode && cached->second.inode == e.inode && cached->second.size) {
                d.size = cached->second.size;
                d.title = cached->second.title;
                d.author = cached->second.author;
                d.metadata = cached->second.metadata;
                d.narrated = cached->second.narrated;
                // A ZIP found to hold a DAISY book stays one.
                if (cached->second.format == "daisy") {
                    d.format = "daisy";
                    d.kind = FileKind::Book;
                }
                ++st.reused;
            } else if (e.size) {
                d.size = e.size;
            } else {
                d.size = lib::file_size(d.path);
                ++st.sizes_read;
            }
            if (d.title.empty()) d.title = title_from_file(e.name);
            d.fingerprint = file_fingerprint(d.path, d.size);
            out.push_back(std::move(d));
            if (out.size() >= kMaxDocuments) {
                st.truncated = true;
                return;
            }
        }
    };
    walk(root.path, "", 0);
    st.documents = out.size();
    return out;
}

void read_metadata(LocalDoc& d) {
    d.metadata = true;
    std::string error;
    const std::string fallback = title_from_file(d.path);
    if (d.format == "epub") {
        doc::Epub book;
        if (book.open(d.path, error)) {
            d.title = book.title();
            d.author = book.author();
            d.narrated = book.has_narration();
        }
    } else if (d.format == "mobi" || d.format == "azw3") {
        // The reader loads the whole file; for the shelf, only reasonably sized ones.
        doc::Mobi book;
        if (d.size <= 64ull * 1024 * 1024 && book.open(d.path, error)) {
            d.title = book.title();
            d.author = book.author();
        }
    } else if (d.format == "fb2") {
        fb2_metadata(d);
    } else if (doc::is_office_format(d.format)) {
        doc::office_metadata(d.path, d.title, d.author);
    } else if (d.format == "zim") {
        if (auto zim = zim::Archive::open(d.path, error)) {
            d.title = zim->metadata("Title");
            d.author = zim->metadata("Creator");
        }
    } else if (d.format == "daisy") {
        d.narrated = doc::daisy_metadata(d.path, d.title, d.author);
        if (!d.narrated) d.title.clear();
    } else if (d.format == "zip" && doc::daisy_metadata(d.path, d.title, d.author)) {
        d.format = "daisy";  // a zipped DAISY book, not a comic
        d.kind = FileKind::Book;
        d.narrated = true;
    } else if (d.format == "pdf") {
        if (auto pdf = doc::open_pdf(d.path, error, 4)) {
            d.title = pdf->title();
            d.author = pdf->author();
        }
    }
    d.title = lib::trim(d.title);
    d.author = lib::trim(d.author);
    // A file's own title can be junk ("Microsoft Word - draft.doc", "untitled").
    if (d.title.empty() || lib::iequals(d.title, "untitled") || lib::icontains(d.title, "Microsoft Word")) d.title = fallback;
}

bool save_index(const std::string& path, const std::vector<LocalDoc>& docs) {
    lib::Json list = lib::Json::array();
    for (const LocalDoc& d : docs) {
        lib::Json j = lib::Json::object();
        j.set("path", d.path);
        j.set("format", d.format);
        j.set("title", d.title);
        if (!d.author.empty()) j.set("author", d.author);
        if (!d.folder.empty()) j.set("folder", d.folder);
        j.set("origin", (int)d.origin);
        j.set("slot", d.slot);
        j.set("size", (int64_t)d.size);
        j.set("inode", (int64_t)d.inode);
        j.set("metadata", d.metadata);
        if (d.narrated) j.set("narrated", true);
        list.push(std::move(j));
    }
    lib::Json root = lib::Json::object();
    // 2: "narrated" recorded (talking books are listed under Audiobooks).
    root.set("version", 2);
    root.set("documents", std::move(list));
    return lib::write_file_atomic(path, root.dump());
}

std::vector<LocalDoc> load_index(const std::string& path) {
    std::vector<LocalDoc> docs;
    std::string text;
    if (!lib::read_file(path, text)) return docs;
    bool ok = false;
    const lib::Json root = lib::Json::parse(text, &ok);
    if (!ok) return docs;  // damaged: rebuilt by the next scan
    for (const lib::Json& j : root["documents"].items()) {
        LocalDoc d;
        d.path = j["path"].str();
        d.format = j["format"].str();
        if (d.path.empty() || !readable_extension(d.format)) continue;
        d.title = j["title"].str();
        d.author = j["author"].str();
        d.folder = j["folder"].str();
        d.origin = (Origin)std::max(0, std::min(2, (int)j["origin"].integer(1)));
        d.slot = (int)j["slot"].integer(-1);
        d.size = (uint64_t)j["size"].integer(0);
        d.inode = (uint64_t)j["inode"].integer(0);
        d.metadata = j["metadata"].boolean(false);
        d.narrated = j["narrated"].boolean(false);
        // An index from before talking books were noted: EPUBs and DAISY
        // books are read once more to find out.
        if (root["version"].integer(1) < 2 && (d.format == "epub" || d.format == "daisy")) d.metadata = false;
        d.kind = kind_of(d.format);
        d.fingerprint = file_fingerprint(d.path, d.size);
        docs.push_back(std::move(d));
    }
    return docs;
}

// What the cover finder may look up for a book without a cover: its own
// title and author (never a title made from the file name).
void note_cover_hint(const LocalDoc& d) {
    if (!d.metadata || d.author.empty() || d.kind == FileKind::Table || d.kind == FileKind::Collection) return;
    if (d.title.empty() || d.title == title_from_file(d.path)) return;
    doc::set_cover_hint(d.path, d.title, d.author);
}

// ---- the service --------------------------------------------------------------------------

LocalLibrary& local_library() {
    static LocalLibrary instance;
    return instance;
}

int LocalLibrary::drives() const {
    int n = 0;
    for (const ScanRoot& r : roots_) n += r.origin == Origin::Usb;
    return n;
}

const LocalDoc* LocalLibrary::find_available(const std::string& fingerprint) const {
    const LocalDoc* best = nullptr;
    for (const LocalDoc& d : docs_)
        if (d.available && d.fingerprint == fingerprint && (!best || d.origin < best->origin)) best = &d;
    return best;
}

std::string LocalLibrary::locate(const std::string& path, const std::string& fingerprint) const {
    bool gone = false;
    for (const LocalDoc& d : docs_)
        if (d.path == path) {
            gone = !d.available;
            break;
        }
    // Not in the index: still unreachable if it is on a USB slot with no drive.
    if (!gone)
        for (const std::string& slot : platform::usb_mount_points())
            if (lib::starts_with(path, slot + "/"))
                gone = std::none_of(roots_.begin(), roots_.end(), [&](const ScanRoot& r) { return r.path == slot; });
    if (!gone) return path;
    if (const LocalDoc* copy = fingerprint.empty() ? nullptr : find_available(fingerprint)) return copy->path;
    return "";
}

void LocalLibrary::changed() {
    ++generation_;
    platform::request_redraw();
}

void LocalLibrary::start() {
    if (started_) return;
    started_ = true;
    docs_ = load_index(index_path());
    for (const LocalDoc& d : docs_) note_cover_hint(d);
    for (LocalDoc& d : docs_) d.available = false;
    LOG("local: %zu documents in the index", docs_.size());
    // Which places are there now (a handful of directory reads): what the index
    // knows about them shows straight away, and is then checked by a scan.
    roots_ = present_roots();
    for (LocalDoc& d : docs_)
        for (const ScanRoot& r : roots_)
            if (lib::starts_with(d.path, r.path + "/")) d.available = true;
    prune();
    changed();
    for (const ScanRoot& r : roots_) scan(r);
    last_poll_ = platform::now_ms();
}

void LocalLibrary::refresh() {
    // Drives removed or added since the last poll first, then everything again.
    const std::vector<ScanRoot> roots = present_roots();
    const std::vector<ScanRoot> before = roots_;
    apply_roots(roots);
    for (const ScanRoot& r : roots_)
        if (std::find(before.begin(), before.end(), r) != before.end()) scan(r);  // new ones are already scanning
    last_poll_ = platform::now_ms();
}

void LocalLibrary::tick() {
    if (!started_ || polling_) return;
    const uint64_t now = platform::now_ms();
    if (now - last_poll_ < kPollMs) return;
    last_poll_ = now;
    polling_ = true;
    lib::async<std::vector<ScanRoot>>(
        nullptr, [] { return present_roots(); },
        [this](std::vector<ScanRoot>& roots) {
            polling_ = false;
            apply_roots(roots);
        },
        0);
}

void LocalLibrary::apply_roots(const std::vector<ScanRoot>& roots) {
    std::vector<ScanRoot> added, removed;
    for (const ScanRoot& r : roots)
        if (std::find(roots_.begin(), roots_.end(), r) == roots_.end()) added.push_back(r);
    for (const ScanRoot& r : roots_)
        if (std::find(roots.begin(), roots.end(), r) == roots.end()) removed.push_back(r);
    if (added.empty() && removed.empty()) return;
    roots_ = roots;
    for (const ScanRoot& r : removed) {
        LOG("local: %s disconnected", r.path.c_str());
        for (LocalDoc& d : docs_)
            if (lib::starts_with(d.path, r.path + "/")) {
                d.available = false;
                // Archives kept open for fast page reads point at a drive that is gone.
                doc::close_archive(d.path);
            }
    }
    if (!removed.empty()) {
        prune();
        changed();
        save_index(index_path(), docs_);
    }
    for (const ScanRoot& r : added) {
        LOG("local: %s connected", r.path.c_str());
        scan(r);
    }
}

bool LocalLibrary::file_present(const LocalDoc& doc) { return lib::file_exists(doc.path); }

void LocalLibrary::note_new_download(const std::string& path) {
    // Make sure the containing place is a known root (a download to a USB
    // drive's folder, or to console storage), then scan it so the new book is
    // picked up with its metadata and cover. Also insert it at once so it shows
    // immediately, before the scan finishes.
    ScanRoot root;
    bool found_root = false;
    for (const ScanRoot& r : present_roots())
        if (lib::starts_with(path, r.path + "/")) { root = r; found_root = true; }
    if (!found_root) {
        LOG("local: downloaded file is not under a known root: %s", path.c_str());
        return;
    }
    // An incomplete or non-book file is not shelved (a download is only noted
    // once it has been renamed from its .part temporary to its final name).
    if (lib::ends_with(path, ".part") || lib::ends_with(path, ".tmp")) return;
    if (!readable_extension(format_of(path))) return;
    if (!lib::file_exists(path)) return;
    const uint64_t size = lib::file_size(path);
    const std::string fp = file_fingerprint(path, size);
    bool known = false;
    for (const LocalDoc& d : docs_)
        if (d.path == path) known = true;
    if (!known) {
        LocalDoc d;
        d.path = path;
        d.format = format_of(path);
        d.kind = kind_of(d.format);
        d.origin = root.origin;
        d.slot = root.slot;
        d.size = size;
        d.fingerprint = fp;
        d.title = title_from_file(path);
        // Drop any stale, disconnected copy of the same book.
        docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                                   [&](const LocalDoc& o) { return !o.available && o.fingerprint == fp; }),
                    docs_.end());
        docs_.push_back(std::move(d));
        changed();
    }
    LOG("local: new download %s", path.c_str());
    scan(root);
}

void LocalLibrary::remove_entry(const std::string& fingerprint) {
    const size_t before = docs_.size();
    docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                               [&](const LocalDoc& d) { return d.fingerprint == fingerprint; }),
                docs_.end());
    if (docs_.size() != before) {
        changed();
        save_index(index_path(), docs_);
    }
}

int LocalLibrary::clean_missing() {
    const size_t before = docs_.size();
    docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                               [](const LocalDoc& d) {
                                   // Only judge files on a connected drive; a
                                   // disconnected drive's books are left alone.
                                   return d.available && !file_present(d);
                               }),
                docs_.end());
    const int removed = (int)(before - docs_.size());
    if (removed) {
        changed();
        save_index(index_path(), docs_);
    }
    LOG("local: cleaned %d missing entries", removed);
    return removed;
}

// Unavailable documents stay listed only if they have been read, bookmarked
// or favourited, so the drive can be plugged back in to carry on.
void LocalLibrary::prune() {
    docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                               [](const LocalDoc& d) {
                                   if (d.available) return false;
                                   const Item* it = library().find_file(d.path, d.fingerprint);
                                   return !it || !(it->opened || it->favourite || !it->bookmarks.empty());
                               }),
                docs_.end());
}

void LocalLibrary::scan(const ScanRoot& root) {
    ++scans_;
    std::map<std::string, LocalDoc> cache;
    for (const LocalDoc& d : docs_)
        if (lib::starts_with(d.path, root.path + "/")) cache[d.path] = d;
    struct Result {
        std::vector<LocalDoc> docs;
        ScanStats stats;
    };
    const uint64_t started = platform::now_ms();
    lib::async<Result>(
        nullptr,
        [root, cache]() {
            Result r;
            r.docs = scan_root(root, cache, &r.stats);
            return r;
        },
        [this, root, started](Result& r) {
            --scans_;
            LOG("local: %s: %zu documents, %zu folders, %zu reused, %zu measured in %llu ms%s", root.path.c_str(),
                r.stats.documents, r.stats.directories, r.stats.reused, r.stats.sizes_read,
                (unsigned long long)(platform::now_ms() - started), r.stats.truncated ? " (limit reached)" : "");
            // The drive may have gone while it was being scanned.
            if (std::find(roots_.begin(), roots_.end(), root) == roots_.end()) return;
            const std::string prefix = root.path + "/";
            docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                                       [&](const LocalDoc& d) { return lib::starts_with(d.path, prefix); }),
                        docs_.end());
            for (LocalDoc& d : r.docs) docs_.push_back(std::move(d));
            // Documents of a drive now in another slot are the same books:
            // the stale entries (unavailable, same fingerprint) go.
            std::map<std::string, bool> here;
            for (const LocalDoc& d : docs_)
                if (d.available) here[d.fingerprint] = true;
            docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                                       [&](const LocalDoc& d) { return !d.available && here.count(d.fingerprint); }),
                        docs_.end());
            prune();
            // Covers that failed while these files could not be reached (the
            // drive was out, or a file was being copied) are tried again.
            gfx::images_retry_failed("cover:" + prefix);
            gfx::images_retry_failed("archive:" + prefix);
            changed();
            save_index(index_path(), docs_);
            read_missing_metadata();
        },
        1);
}

// Titles and authors from the files themselves, a batch at a time.
void LocalLibrary::read_missing_metadata() {
    if (metadata_running_) return;
    std::vector<LocalDoc> batch;
    for (const LocalDoc& d : docs_)
        if (d.available && !d.metadata && batch.size() < 24) batch.push_back(d);
    if (batch.empty()) return;
    metadata_running_ = true;
    lib::async<std::vector<LocalDoc>>(
        nullptr,
        [batch]() mutable {
            for (LocalDoc& d : batch) read_metadata(d);
            return batch;
        },
        [this](std::vector<LocalDoc>& done) {
            metadata_running_ = false;
            for (const LocalDoc& m : done)
                for (LocalDoc& d : docs_)
                    if (d.path == m.path && d.size == m.size) {
                        d.title = m.title;
                        d.author = m.author;
                        d.format = m.format;
                        d.kind = m.kind;
                        d.narrated = m.narrated;
                        d.metadata = true;
                        note_cover_hint(d);
                    }
            changed();
            save_index(index_path(), docs_);
            read_missing_metadata();  // the next batch
        },
        0);
}

}  // namespace svc
