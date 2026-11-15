// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// ZIM files (Kiwix collections), read directly: the header, directory entries,
// path and title lists, and clusters (uncompressed, xz or zstd). Both the old
// namespace layout (A/ articles, I/ images, -/ styles; ZIM 5 and 6.0) and the
// new one (everything under C/; ZIM 6.1+) are read. Full-text search (the
// Xapian index inside the file) is not.
//
// Files are read through lib::open_file, so ZIMs on a USB drive work through
// GoldHEN's FTP server too (slowly: every lookup is a few small reads).
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#include <list>
#include <memory>
#include <string>
#include <vector>

namespace zim {

struct Entry {
    uint32_t index = 0;
    char ns = 0;
    std::string path, title;  // title: the path when the file gives none
    uint16_t mime = 0;        // index into the MIME list; 0xFFFF redirect
    uint32_t cluster = 0, blob = 0;
    uint32_t redirect = 0;    // the target's index, for redirects
    bool is_redirect() const { return mime == 0xFFFF; }
};

class Archive {
public:
    static std::shared_ptr<Archive> open(const std::string& path, std::string& error);
    ~Archive();

    const std::string& path() const { return path_; }
    uint32_t entry_count() const { return entry_count_; }
    // The new layout: content in C/, no namespace in addresses.
    bool new_namespace() const { return new_ns_; }
    // The namespace articles are in ('C' or 'A').
    char content_ns() const { return new_ns_ ? 'C' : 'A'; }

    bool entry(uint32_t index, Entry& out);
    bool find(char ns, const std::string& path, Entry& out);
    // Follows redirects (a few hops at most).
    bool resolve(Entry& e);
    // The data of an entry (redirects followed) and its MIME type.
    bool read(Entry e, std::string& data, std::string& mime);
    std::string mime_name(uint16_t mime) const;
    bool is_html(const Entry& e) const;
    bool main_page(Entry& out);
    // M/<name>: Title, Description, Language, Creator, Date, Counter...
    std::string metadata(const std::string& name);
    // A small picture for the collection (48 px PNG), when it has one.
    bool illustration(std::string& png);
    // Articles whose title starts with `prefix` (as typed, then with its first
    // letter in the other case).
    void suggest(const std::string& prefix, size_t count, std::vector<Entry>& out);
    bool random_article(Entry& out);
    // How many articles (from M/Counter; 0 when unknown).
    int64_t article_count();

private:
    Archive() = default;
    bool read_at(uint64_t offset, void* data, size_t size);
    bool read_some(uint64_t offset, std::string& out, size_t max);
    void setup_titles();
    bool title_entry(uint32_t position, Entry& out);  // position in the title list
    uint32_t title_lower_bound(char ns, const std::string& title);
    bool blob(uint32_t cluster, uint32_t index, std::string& out);
    bool cluster_data(uint32_t cluster, std::shared_ptr<const std::string>& data, bool& extended);
    uint64_t cluster_end(uint32_t cluster);

    std::string path_;
    FILE* file_ = nullptr;
    uint64_t file_size_ = 0;
    uint32_t entry_count_ = 0, cluster_count_ = 0, main_page_ = 0xFFFFFFFFu;
    uint64_t path_ptr_ = 0, title_ptr_ = 0, cluster_ptr_ = 0, checksum_pos_ = 0;
    bool new_ns_ = false;
    // The title list: the header's (all entries, by namespace and title), or in
    // newer files X/listing/titleOrdered/v1 (articles only, by title), read in
    // place when its cluster is uncompressed, else held in memory.
    uint32_t title_count_ = 0;
    uint64_t title_offset_ = 0;
    bool titles_by_ns_ = true;
    std::string title_memory_;
    std::vector<std::string> mimes_;
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
    struct CachedCluster {
        uint32_t index;
        bool extended;
        std::shared_ptr<const std::string> data;
    };
    std::list<CachedCluster> clusters_;  // most recent first
};

// Decompressors (exposed for tests): the whole of `in` into `out`, at most `limit` bytes.
bool unzstd(const std::string& in, std::string& out, size_t limit);
bool unxz(const std::string& in, std::string& out, size_t limit);

}  // namespace zim
