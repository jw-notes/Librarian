// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The ZIM format as openZIM documents it (https://wiki.openzim.org/wiki/ZIM_file_format):
// an 80-byte header; a MIME type list; path and title pointer lists; directory
// entries; clusters of blobs, each cluster uncompressed (1), xz (4) or zstd (5),
// with 32-bit blob offsets, or 64-bit ones when bit 4 of its first byte is set.
#include "doc/zim.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"

// Must match how third_party/xz is built (the Makefile's XZ_FLAGS).
#define XZ_USE_CRC64
extern "C" {
#include "xz/xz.h"
#include "zstd/zstd.h"
}

namespace zim {

namespace {

constexpr uint32_t kMagic = 0x044D495A;
// One decompressed cluster may not exceed this (the console has ~400 MB).
constexpr size_t kMaxCluster = 96u * 1024 * 1024;
constexpr size_t kCachedClusters = 2;

uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t u32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t u64(const unsigned char* p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

struct Locked {
    explicit Locked(pthread_mutex_t* m) : m_(m) { pthread_mutex_lock(m_); }
    ~Locked() { pthread_mutex_unlock(m_); }
    pthread_mutex_t* m_;
};

// Compares (ns, text) pairs as the ZIM lists are sorted: by bytes.
int compare(char ns_a, const std::string& a, char ns_b, const std::string& b) {
    if (ns_a != ns_b) return (unsigned char)ns_a < (unsigned char)ns_b ? -1 : 1;
    return a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0;
}

}  // namespace

bool unzstd(const std::string& in, std::string& out, size_t limit) {
    out.clear();
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return false;
    ZSTD_initDStream(ds);
    ZSTD_inBuffer input{in.data(), in.size(), 0};
    std::string chunk(256 * 1024, '\0');
    bool ok = true;
    while (input.pos < input.size) {
        ZSTD_outBuffer output{&chunk[0], chunk.size(), 0};
        const size_t rc = ZSTD_decompressStream(ds, &output, &input);
        if (ZSTD_isError(rc)) {
            ok = false;
            break;
        }
        out.append(chunk.data(), output.pos);
        if (out.size() > limit) {
            ok = false;
            break;
        }
        if (rc == 0) break;  // the frame is complete (a cluster is one frame)
        if (output.pos == 0 && input.pos == input.size) break;
    }
    // Data still buffered inside the decoder.
    for (int i = 0; ok && i < 1 << 16; ++i) {
        ZSTD_outBuffer output{&chunk[0], chunk.size(), 0};
        const size_t rc = ZSTD_decompressStream(ds, &output, &input);
        if (ZSTD_isError(rc)) ok = false;
        out.append(chunk.data(), output.pos);
        if (out.size() > limit) ok = false;
        if (output.pos < chunk.size()) break;
    }
    ZSTD_freeDStream(ds);
    return ok && !out.empty();
}

bool unxz(const std::string& in, std::string& out, size_t limit) {
    static bool crc_ready = false;
    if (!crc_ready) {
        xz_crc32_init();
#ifdef XZ_USE_CRC64
        xz_crc64_init();
#endif
        crc_ready = true;
    }
    out.clear();
    xz_dec* dec = xz_dec_init(XZ_DYNALLOC, 1u << 27);
    if (!dec) return false;
    std::string chunk(256 * 1024, '\0');
    xz_buf b{};
    b.in = (const uint8_t*)in.data();
    b.in_size = in.size();
    bool ok = false;
    for (;;) {
        b.out = (uint8_t*)&chunk[0];
        b.out_pos = 0;
        b.out_size = chunk.size();
        const xz_ret rc = xz_dec_run(dec, &b);
        out.append(chunk.data(), b.out_pos);
        if (out.size() > limit) break;
        if (rc == XZ_STREAM_END) {
            ok = true;
            break;
        }
        if (rc != XZ_OK && rc != XZ_UNSUPPORTED_CHECK) break;
        if (b.out_pos == 0 && b.in_pos == b.in_size) break;  // truncated
    }
    xz_dec_end(dec);
    return ok;
}

std::shared_ptr<Archive> Archive::open(const std::string& path, std::string& error) {
    std::shared_ptr<Archive> a(new Archive());
    a->path_ = path;
    a->file_ = lib::open_file(path, "rb");
    if (!a->file_) {
        error = "The file could not be opened.";
        return nullptr;
    }
    a->file_size_ = lib::file_size(path);
    unsigned char h[80];
    if (!a->read_at(0, h, sizeof h) || u32(h) != kMagic) {
        error = "This is not a ZIM file.";
        return nullptr;
    }
    const uint16_t major = u16(h + 4), minor = u16(h + 6);
    a->entry_count_ = u32(h + 24);
    a->cluster_count_ = u32(h + 28);
    a->path_ptr_ = u64(h + 32);
    a->title_ptr_ = u64(h + 40);
    a->cluster_ptr_ = u64(h + 48);
    const uint64_t mime_pos = u64(h + 56);
    a->main_page_ = u32(h + 64);
    a->checksum_pos_ = u64(h + 72);
    a->new_ns_ = major >= 6 && minor >= 1;
    if (major < 5 || major > 6 || !a->entry_count_) {
        error = "This ZIM file's version (" + std::to_string(major) + "." + std::to_string(minor) + ") is not supported.";
        return nullptr;
    }
    // MIME types: zero-terminated strings, ending with an empty one.
    std::string list;
    if (!a->read_some(mime_pos, list, 64 * 1024)) {
        error = "The file is damaged (MIME list).";
        return nullptr;
    }
    for (size_t at = 0; at < list.size();) {
        const size_t end = list.find('\0', at);
        if (end == std::string::npos || end == at) break;
        a->mimes_.push_back(list.substr(at, end - at));
        at = end + 1;
    }
    a->setup_titles();
    return a;
}

Archive::~Archive() {
    if (file_) fclose(file_);
}

bool Archive::read_at(uint64_t offset, void* data, size_t size) {
    if (fseeko(file_, (off_t)offset, SEEK_SET) != 0) return false;
    return fread(data, 1, size, file_) == size;
}

bool Archive::read_some(uint64_t offset, std::string& out, size_t max) {
    if (file_size_ && offset >= file_size_) return false;
    if (file_size_) max = (size_t)std::min<uint64_t>(max, file_size_ - offset);
    out.resize(max);
    if (fseeko(file_, (off_t)offset, SEEK_SET) != 0) return false;
    const size_t n = fread(&out[0], 1, max, file_);
    out.resize(n);
    return n > 0;
}

bool Archive::entry(uint32_t index, Entry& out) {
    Locked lock(&lock_);
    if (index >= entry_count_) return false;
    unsigned char p[8];
    if (!read_at(path_ptr_ + 8ull * index, p, 8)) return false;
    std::string d;
    // A directory entry is short; a long path or title needs a second read.
    for (size_t want = 512; want <= 64 * 1024; want *= 8) {
        if (!read_some(u64(p), d, want) || d.size() < 12) return false;
        const unsigned char* b = (const unsigned char*)d.data();
        out.index = index;
        out.mime = u16(b);
        const size_t param = b[2];
        out.ns = (char)b[3];
        size_t at;
        if (out.mime == 0xFFFF) {
            out.redirect = u32(b + 8);
            at = 12;
        } else {
            if (d.size() < 16) return false;
            out.cluster = u32(b + 8);
            out.blob = u32(b + 12);
            at = 16;
        }
        const size_t path_end = d.find('\0', at);
        if (path_end == std::string::npos) continue;
        const size_t title_end = d.find('\0', path_end + 1);
        if (title_end == std::string::npos || title_end + 1 + param > d.size()) {
            if (d.size() < want) return false;  // the end of the file
            continue;
        }
        out.path = d.substr(at, path_end - at);
        out.title = d.substr(path_end + 1, title_end - path_end - 1);
        if (out.title.empty()) out.title = out.path;
        return true;
    }
    return false;
}

bool Archive::find(char ns, const std::string& path, Entry& out) {
    uint32_t lo = 0, hi = entry_count_;
    Entry e;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (!entry(mid, e)) return false;
        const int c = compare(e.ns, e.path, ns, path);
        if (c == 0) {
            out = e;
            return true;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

bool Archive::resolve(Entry& e) {
    for (int hop = 0; e.is_redirect() && hop < 8; ++hop)
        if (!entry(e.redirect, e)) return false;
    return !e.is_redirect() && e.mime < 0xFFFD;
}

std::string Archive::mime_name(uint16_t mime) const { return mime < mimes_.size() ? mimes_[mime] : std::string(); }

bool Archive::is_html(const Entry& e) const {
    return !e.is_redirect() && lib::starts_with(mime_name(e.mime), "text/html");
}

uint64_t Archive::cluster_end(uint32_t cluster) {
    if (cluster + 1 < cluster_count_) {
        unsigned char p[8];
        if (read_at(cluster_ptr_ + 8ull * (cluster + 1), p, 8)) return u64(p);
    }
    return checksum_pos_ ? checksum_pos_ : file_size_;
}

bool Archive::cluster_data(uint32_t cluster, std::shared_ptr<const std::string>& data, bool& extended) {
    for (auto it = clusters_.begin(); it != clusters_.end(); ++it)
        if (it->index == cluster) {
            clusters_.splice(clusters_.begin(), clusters_, it);
            data = clusters_.front().data;
            extended = clusters_.front().extended;
            return true;
        }
    unsigned char p[8];
    if (cluster >= cluster_count_ || !read_at(cluster_ptr_ + 8ull * cluster, p, 8)) return false;
    const uint64_t start = u64(p), end = cluster_end(cluster);
    if (end <= start + 1 || end - start > kMaxCluster) {
        LOG("zim: cluster %u of %s is too large or damaged", cluster, lib::file_name(path_).c_str());
        return false;
    }
    std::string raw;
    raw.resize((size_t)(end - start));
    if (!read_at(start, &raw[0], raw.size())) return false;
    const unsigned char info = (unsigned char)raw[0];
    extended = (info & 0x10) != 0;
    const int compression = info & 0x0F;
    auto out = std::make_shared<std::string>();
    raw.erase(0, 1);
    bool ok = true;
    if (compression <= 1) *out = std::move(raw);
    else if (compression == 5) ok = unzstd(raw, *out, kMaxCluster);
    else if (compression == 4) ok = unxz(raw, *out, kMaxCluster);
    else ok = false;  // zlib and bzip2 clusters (very old files) are not read
    if (!ok) {
        LOG("zim: cluster %u of %s not read (compression %d)", cluster, lib::file_name(path_).c_str(), compression);
        return false;
    }
    clusters_.push_front({cluster, extended, out});
    while (clusters_.size() > kCachedClusters) clusters_.pop_back();
    data = out;
    return true;
}

bool Archive::blob(uint32_t cluster, uint32_t index, std::string& out) {
    std::shared_ptr<const std::string> data;
    bool extended = false;
    if (!cluster_data(cluster, data, extended)) return false;
    const size_t w = extended ? 8 : 4;
    const std::string& d = *data;
    if (d.size() < w) return false;
    auto offset = [&](size_t i) -> uint64_t {
        const unsigned char* q = (const unsigned char*)d.data() + i * w;
        return extended ? u64(q) : u32(q);
    };
    const uint64_t first = offset(0);
    const size_t blobs = first / w ? (size_t)(first / w) - 1 : 0;
    if (index >= blobs || (index + 2) * w > d.size()) return false;
    const uint64_t a = offset(index), b = offset(index + 1);
    if (a > b || b > d.size()) return false;
    out.assign(d, (size_t)a, (size_t)(b - a));
    return true;
}

bool Archive::read(Entry e, std::string& data, std::string& mime) {
    if (!resolve(e)) return false;
    mime = mime_name(e.mime);
    Locked lock(&lock_);
    return blob(e.cluster, e.blob, data);
}

bool Archive::main_page(Entry& out) {
    if (new_ns_ && find('W', "mainPage", out)) return true;
    return main_page_ != 0xFFFFFFFFu && entry(main_page_, out);
}

std::string Archive::metadata(const std::string& name) {
    Entry e;
    std::string data, mime;
    if (!find('M', name, e) || !read(e, data, mime)) return std::string();
    return data;
}

bool Archive::illustration(std::string& png) {
    std::string mime;
    Entry e;
    if (find('M', "Illustration_48x48@1", e) && read(e, png, mime)) return true;
    if (!new_ns_ && find('-', "favicon", e) && read(e, png, mime)) return true;
    return false;
}

int64_t Archive::article_count() {
    // "text/html=1234;image/png=56;..."
    const std::string counter = metadata("Counter");
    size_t at = counter.find("text/html=");
    if (at == std::string::npos) return 0;
    return atoll(counter.c_str() + at + 10);
}

void Archive::setup_titles() {
    const bool header_list = title_ptr_ && title_ptr_ != ~0ull && title_ptr_ + 4ull * entry_count_ <= file_size_;
    Entry e;
    if (new_ns_ && find('X', "listing/titleOrdered/v1", e) && resolve(e)) {
        Locked lock(&lock_);
        unsigned char p[8];
        if (e.cluster < cluster_count_ && read_at(cluster_ptr_ + 8ull * e.cluster, p, 8)) {
            const uint64_t start = u64(p);
            unsigned char info = 0;
            read_at(start, &info, 1);
            if ((info & 0x0F) <= 1) {
                // Uncompressed: read where it is (tens of MB for a large Wikipedia).
                const size_t w = (info & 0x10) ? 8 : 4;
                unsigned char o[16];
                if (read_at(start + 1 + w * e.blob, o, 2 * w)) {
                    const uint64_t a = w == 8 ? u64(o) : u32(o), b = w == 8 ? u64(o + 8) : u32(o + 4);
                    if (b > a) {
                        title_offset_ = start + 1 + a;
                        title_count_ = (uint32_t)((b - a) / 4);
                        titles_by_ns_ = false;
                        return;
                    }
                }
            } else if (blob(e.cluster, e.blob, title_memory_) && title_memory_.size() >= 4) {
                title_count_ = (uint32_t)(title_memory_.size() / 4);
                titles_by_ns_ = false;
                return;
            }
        }
    }
    if (header_list) {
        title_offset_ = title_ptr_;
        title_count_ = entry_count_;
    }
}

bool Archive::title_entry(uint32_t position, Entry& out) {
    unsigned char p[4];
    if (position >= title_count_) return false;
    if (!title_memory_.empty()) {
        memcpy(p, title_memory_.data() + 4ull * position, 4);
    } else {
        Locked lock(&lock_);
        if (!read_at(title_offset_ + 4ull * position, p, 4)) return false;
    }
    return entry(u32(p), out);
}

uint32_t Archive::title_lower_bound(char ns, const std::string& title) {
    uint32_t lo = 0, hi = title_count_;
    Entry e;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (!title_entry(mid, e)) return title_count_;
        // The newer list holds articles only, sorted by title alone.
        const int c = titles_by_ns_ ? compare(e.ns, e.title, ns, title) : compare(0, e.title, 0, title);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void Archive::suggest(const std::string& prefix_in, size_t count, std::vector<Entry>& out) {
    out.clear();
    const std::string prefix = lib::trim(prefix_in);
    if (prefix.empty() || !title_count_) return;
    // The list is sorted by bytes, so a few spellings are looked up: as typed,
    // first letter capitalised, every word capitalised, lower case, upper case.
    auto upper = [](char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; };
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; };
    std::string low = prefix, words = prefix, up = prefix;
    for (char& c : low) c = lower(c);
    std::string first = low;
    first[0] = upper(first[0]);
    for (size_t i = 0; i < words.size(); ++i) words[i] = i == 0 || words[i - 1] == ' ' ? upper(words[i]) : lower(words[i]);
    for (char& c : up) c = upper(c);
    std::vector<std::string> tries;
    for (const std::string& t : {prefix, first, words, low, up})
        if (std::find(tries.begin(), tries.end(), t) == tries.end()) tries.push_back(t);
    const char ns = content_ns();
    for (const std::string& p : tries) {
        Entry e;
        // Redirects count (other names of an article), but at most a few
        // hundred entries are looked at.
        for (uint32_t i = title_lower_bound(ns, p), seen = 0; i < title_count_ && out.size() < count && seen < 400; ++i, ++seen) {
            if (!title_entry(i, e) || e.ns != ns || !lib::starts_with(e.title, p)) break;
            Entry target = e;
            if (!resolve(target) || !is_html(target)) continue;
            bool dup = false;
            for (const auto& o : out) dup = dup || o.path == e.path;
            if (!dup) out.push_back(e);
        }
    }
}

bool Archive::random_article(Entry& out) {
    for (int tries = 0; tries < 64; ++tries) {
        const uint32_t i = (uint32_t)((((uint64_t)rand() << 16) ^ (uint64_t)rand()) % entry_count_);
        if (entry(i, out) && out.ns == content_ns() && is_html(out)) return true;
    }
    return false;
}

}  // namespace zim
