// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The Local Library: every readable document in Librarian's downloads, the
// console's /data/books and connected USB drives, found automatically.
//
// - USB slots are polled every few seconds; a drive that appears is scanned,
//   one that disappears has its documents marked unavailable.
// - What was found is kept in local_index.json, so the shelf appears at once
//   on the next start, and unchanged files are not measured or opened again.
// - Documents are identified by file name and size (their fingerprint), so a
//   drive that comes back in another slot keeps its books' reading progress.
// - Titles and authors are read from the files in the background.
#include <stdint.h>

#include <map>
#include <algorithm>
#include <string>
#include <vector>

#include "services/local_files.h"

namespace svc {

enum class Origin { Downloaded = 0, Internal = 1, Usb = 2 };

struct LocalDoc {
    std::string path, format, fingerprint;
    std::string title, author;  // from the file, or the file name
    std::string folder;         // where it is, relative to its drive or folder ("Books/Tolkien")
    FileKind kind = FileKind::Book;
    Origin origin = Origin::Internal;
    int slot = -1;              // USB slot 0..7
    uint64_t size = 0, inode = 0;
    bool metadata = false;      // title/author have been read from the file
    bool narrated = false;      // a talking book: EPUB 3 with narration, or DAISY (with metadata)
    bool available = true;      // false: on a drive that is not connected

    std::string place() const;  // "Downloaded", "Internal", "USB 1"
};

struct ScanRoot {
    std::string path;
    Origin origin = Origin::Internal;
    int slot = -1;
    bool operator==(const ScanRoot& o) const { return path == o.path && origin == o.origin && slot == o.slot; }
};

struct ScanStats {
    size_t directories = 0, documents = 0;
    size_t sizes_read = 0;     // files that had to be opened to learn their size
    size_t reused = 0;         // documents taken from the cache unchanged
    bool truncated = false;    // limits reached
};

// ---- the parts the service is built from (used directly by tests) ----------------------

// The roots present now: downloads, internal folders that exist, USB slots with a drive.
std::vector<ScanRoot> present_roots();
// Walks a root. `cache` (by path) supplies sizes and metadata for unchanged files.
std::vector<LocalDoc> scan_root(const ScanRoot& root, const std::map<std::string, LocalDoc>& cache, ScanStats* stats = nullptr);
// Reads title and author from the file (EPUB, MOBI/AZW3, FB2, PDF); safe on damaged files.
void read_metadata(LocalDoc& doc);
bool save_index(const std::string& path, const std::vector<LocalDoc>& docs);
std::vector<LocalDoc> load_index(const std::string& path);

class LocalLibrary {
public:
    // Loads the index and starts scanning (once, at launch).
    void start();
    // On the UI thread, every frame: polls the USB slots now and then.
    void tick();
    // Rescans everything (the manual Refresh).
    void refresh();

    const std::vector<LocalDoc>& docs() const { return docs_; }
    // Talking books that can be read now (EPUB 3 with narration, DAISY), by title.
    std::vector<const LocalDoc*> talking_books() const {
        std::vector<const LocalDoc*> out;
        for (const LocalDoc& d : docs_)
            if (d.narrated && d.available) out.push_back(&d);
        std::sort(out.begin(), out.end(), [](const LocalDoc* a, const LocalDoc* b) { return a->title < b->title; });
        return out;
    }
    // Changes whenever docs() changes.
    uint64_t generation() const { return generation_; }
    bool scanning() const { return scans_ > 0 || metadata_running_; }
    int drives() const;
    // An available copy of a document, by fingerprint (a moved or remounted book).
    const LocalDoc* find_available(const std::string& fingerprint) const;
    // Where the document last seen at `path` can be read now, judged from what
    // is known (no disk access): `path` itself, a copy elsewhere with the same
    // fingerprint, or empty when it is on a drive that is not connected.
    std::string locate(const std::string& path, const std::string& fingerprint) const;

    // A book just downloaded to `path`: add it and scan its folder, so it
    // appears without a manual refresh. Runs on the UI thread.
    void note_new_download(const std::string& path);
    // Removes a document from the catalogue (not the file). Any copy with the
    // same fingerprint goes. Removed stale entries stay removed.
    void remove_entry(const std::string& fingerprint);
    // Removes every indexed document whose file is missing on a connected
    // drive (a disconnected drive's books are left alone). Returns the count.
    int clean_missing();
    // True when the file backing this document exists right now.
    static bool file_present(const LocalDoc& doc);

private:
    void scan(const ScanRoot& root);
    void apply_roots(const std::vector<ScanRoot>& roots);
    void prune();
    void read_missing_metadata();
    void changed();

    std::vector<LocalDoc> docs_;
    std::vector<ScanRoot> roots_;
    uint64_t generation_ = 0, last_poll_ = 0;
    int scans_ = 0;
    bool polling_ = false, metadata_running_ = false, started_ = false;
};

LocalLibrary& local_library();

}  // namespace svc
