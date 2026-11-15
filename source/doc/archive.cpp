// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/archive.h"

#include <pthread.h>
#include <string.h>

#include <algorithm>
#include <list>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "miniz.h"

namespace doc {

namespace {

class ZipArchive : public Archive {
public:
    ~ZipArchive() override {
        if (open_) mz_zip_reader_end(&zip_);
        if (file_) fclose(file_);
        pthread_mutex_destroy(&lock_);
    }

    bool open(const std::string& path, std::string* error) {
        memset(&zip_, 0, sizeof zip_);
        // Through lib::open_file, so a drive read over FTP works too (miniz
        // finds the size by seeking and leaves the FILE to us).
        file_ = lib::open_file(path);
        if (!file_ || !mz_zip_reader_init_cfile(&zip_, file_, 0, 0)) {
            if (error) *error = "Not a ZIP archive, or it is damaged.";
            return false;
        }
        open_ = true;
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip_, i, &st)) continue;
            entries_.push_back({st.m_filename, (uint64_t)st.m_uncomp_size, mz_zip_reader_is_file_a_directory(&zip_, i) != 0});
        }
        return true;
    }

    const std::vector<ArchiveEntry>& entries() const override { return entries_; }

    bool read(const std::string& name, std::string& out, uint64_t max_bytes) override {
        out.clear();
        int index = find(name);
        if (index < 0) return false;
        if (entries_[index].size > max_bytes) return false;
        pthread_mutex_lock(&lock_);
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip_, (mz_uint)index, &size, 0);
        pthread_mutex_unlock(&lock_);
        if (!data) return false;
        out.assign((const char*)data, size);
        mz_free(data);
        return true;
    }

    bool stored_span(const std::string& name, uint64_t& offset, uint64_t& size) override {
        const int index = find(name);
        if (index < 0) return false;
        pthread_mutex_lock(&lock_);
        mz_zip_archive_file_stat st;
        bool ok = mz_zip_reader_file_stat(&zip_, (mz_uint)index, &st) && st.m_method == 0 && !st.m_is_encrypted &&
                  st.m_comp_size == st.m_uncomp_size;
        uint8_t h[30];
        // The data follows the local header, whose name and extra field
        // lengths may differ from the central directory's.
        ok = ok && fseeko(file_, (off_t)st.m_local_header_ofs, SEEK_SET) == 0 && fread(h, 1, 30, file_) == 30 &&
             h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4;
        if (ok) {
            offset = st.m_local_header_ofs + 30 + (h[26] | (h[27] << 8)) + (h[28] | (h[29] << 8));
            size = st.m_uncomp_size;
        }
        pthread_mutex_unlock(&lock_);
        return ok;
    }

    bool extract_to(const std::string& name, const std::string& path) override {
        const int index = find(name);
        if (index < 0) return false;
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) return false;
        pthread_mutex_lock(&lock_);
        const bool ok = mz_zip_reader_extract_to_callback(
            &zip_, (mz_uint)index,
            [](void* user, mz_uint64, const void* data, size_t n) { return fwrite(data, 1, n, (FILE*)user); }, out, 0);
        pthread_mutex_unlock(&lock_);
        const bool closed = fclose(out) == 0;
        if (!ok || !closed) remove(path.c_str());
        return ok && closed;
    }

private:
    int find(const std::string& name) const {
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].name == name) return (int)i;
        for (size_t i = 0; i < entries_.size(); ++i)
            if (lib::iequals(entries_[i].name, name)) return (int)i;
        return -1;
    }

    mz_zip_archive zip_;
    FILE* file_ = nullptr;
    bool open_ = false;
    std::vector<ArchiveEntry> entries_;
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
};

pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
std::list<std::pair<std::string, std::shared_ptr<Archive>>> g_cache;  // most recent first

}  // namespace

std::shared_ptr<Archive> open_rar_archive(const std::string& path, std::string* error);

bool Archive::extract_to(const std::string& name, const std::string& path) {
    std::string data;
    if (!read(name, data)) return false;
    FILE* out = fopen(path.c_str(), "wb");
    if (!out) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), out) == data.size();
    return fclose(out) == 0 && ok;
}

bool Archive::has(const std::string& name) const {
    for (const auto& e : entries())
        if (e.name == name || lib::iequals(e.name, name)) return true;
    return false;
}

uint64_t Archive::size_of(const std::string& name) const {
    for (const auto& e : entries())
        if (e.name == name || lib::iequals(e.name, name)) return e.size;
    return 0;
}

std::shared_ptr<Archive> open_archive(const std::string& path, std::string* error) {
    pthread_mutex_lock(&g_cache_lock);
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
        if (it->first == path) {
            auto hit = *it;
            g_cache.erase(it);
            g_cache.push_front(hit);
            pthread_mutex_unlock(&g_cache_lock);
            return hit.second;
        }
    }
    pthread_mutex_unlock(&g_cache_lock);

    std::shared_ptr<Archive> archive;
    auto zip = std::make_shared<ZipArchive>();
    if (zip->open(path, error)) archive = zip;
    else archive = open_rar_archive(path, error);
    if (!archive) return nullptr;

    pthread_mutex_lock(&g_cache_lock);
    g_cache.push_front({path, archive});
    while (g_cache.size() > 4) g_cache.pop_back();
    pthread_mutex_unlock(&g_cache_lock);
    return archive;
}

void close_archive(const std::string& path) {
    pthread_mutex_lock(&g_cache_lock);
    g_cache.remove_if([&](const auto& e) { return e.first == path; });
    pthread_mutex_unlock(&g_cache_lock);
}

std::string archive_resolve(const std::string& base, const std::string& href) {
    std::string h = lib::url_decode(href);
    const size_t hash = h.find('#');
    std::string fragment;
    if (hash != std::string::npos) {
        fragment = h.substr(hash);
        h.resize(hash);
    }
    std::string path;
    if (!h.empty() && h[0] == '/') {
        path = h.substr(1);
    } else {
        const size_t slash = base.rfind('/');
        path = (slash == std::string::npos ? "" : base.substr(0, slash + 1)) + h;
    }
    std::vector<std::string> parts;
    for (const auto& seg : lib::split(path, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(seg);
        }
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    return out + fragment;
}

}  // namespace doc
