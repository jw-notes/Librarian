// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "audio/byte_source.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <list>

#include "core/fs.h"
#include "core/log.h"
#include "net/http.h"
#include "platform/platform.h"

namespace audio {

namespace {

// A local file, through a small read buffer (the parsers read a few bytes at a time).
class FileSource : public ByteSource {
public:
    ~FileSource() override {
        if (f_) fclose(f_);
    }
    bool open(const std::string& path, uint64_t offset, uint64_t length) {
        f_ = lib::open_file(path);
        if (!f_) return false;
        // Sizes by seeking: stat() cannot be trusted on the console (see core/fs.cpp).
        if (fseeko(f_, 0, SEEK_END) != 0) return false;
        const off_t end = ftello(f_);
        size_ = end > 0 ? (uint64_t)end : 0;
        if (length) {
            if (offset >= size_) return false;
            base_ = offset;
            size_ = std::min(length, size_ - offset);
        }
        return true;
    }
    uint64_t size() const override { return size_; }
    size_t read_at(uint64_t offset, void* out, size_t n) override {
        if (offset >= size_) return 0;
        n = (size_t)std::min<uint64_t>(n, size_ - offset);
        size_t done = 0;
        while (done < n) {
            const uint64_t at = offset + done;
            if (at >= buffer_at_ && at < buffer_at_ + buffer_len_) {
                const size_t k = std::min(n - done, (size_t)(buffer_at_ + buffer_len_ - at));
                memcpy((char*)out + done, buffer_ + (at - buffer_at_), k);
                done += k;
                continue;
            }
            if (fseeko(f_, (off_t)(base_ + at), SEEK_SET) != 0) break;
            buffer_at_ = at;
            buffer_len_ = fread(buffer_, 1, sizeof buffer_, f_);
            if (buffer_len_ == 0) {
                error_ = "The file could not be read.";
                break;
            }
        }
        return done;
    }

private:
    FILE* f_ = nullptr;
    uint64_t size_ = 0, buffer_at_ = 0, base_ = 0;
    size_t buffer_len_ = 0;
    char buffer_[64 * 1024];
};

// A file on a server, read in blocks with Range requests.
class HttpSource : public ByteSource {
public:
    HttpSource(std::string url, uint64_t size, HeaderFn headers)
        : url_(std::move(url)), size_(size), headers_(std::move(headers)) {}
    uint64_t size() const override { return size_; }

    size_t read_at(uint64_t offset, void* out, size_t n) override {
        if (offset >= size_) return 0;
        n = (size_t)std::min<uint64_t>(n, size_ - offset);
        size_t done = 0;
        while (done < n) {
            const uint64_t at = offset + done;
            const std::string* block = get_block(at / kBlock);
            if (!block) break;
            const size_t in_block = (size_t)(at % kBlock);
            if (in_block >= block->size()) break;
            const size_t k = std::min(n - done, block->size() - in_block);
            memcpy((char*)out + done, block->data() + in_block, k);
            done += k;
        }
        return done;
    }

private:
    // 256 KB blocks, a dozen kept: about 3 MB per open file however long it is.
    static constexpr uint64_t kBlock = 256 * 1024;
    static constexpr size_t kKeep = 12;

    const std::string* get_block(uint64_t index) {
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it)
            if (it->first == index) {
                blocks_.splice(blocks_.begin(), blocks_, it);
                return &blocks_.front().second;
            }
        const uint64_t from = index * kBlock, to = std::min(size_, from + kBlock) - 1;
        std::string data;
        bool rejected = false, renewed = false;
        for (int attempt = 0; attempt < 4 && !cancelled_; ++attempt) {
            if (attempt && !rejected) platform::sleep_ms(500 * attempt);
            if (cancelled_) break;
            lib::HttpRequest request;
            request.url = url_;
            request.no_auto_auth = true;
            request.timeout_seconds = 20;
            request.max_bytes = kBlock + 1024;
            if (headers_) request.headers = headers_(rejected);
            rejected = false;
            request.headers.emplace_back("Range", "bytes=" + std::to_string(from) + "-" + std::to_string(to));
            lib::HttpResponse r = lib::http_request(request);
            if (r.status == 206 || (r.status == 200 && from == 0)) {
                data = std::move(r.body);
                if (data.size() > to - from + 1) data.resize(to - from + 1);  // a 200 sends everything
                if (!data.empty()) break;
            }
            if (r.status == 200) {
                error_ = "The server does not allow reading part of a file (no Range support).";
                return nullptr;
            }
            error_ = lib::describe_failure(r);
            if (r.status == 401 && !renewed) {
                // An expired sign-in: renew it once and ask again.
                rejected = renewed = true;
                continue;
            }
            if (r.status == 401 || r.status == 403 || r.status == 404) break;  // retrying will not help
            data.clear();
        }
        if (data.empty()) {
            if (cancelled_) error_ = "Cancelled.";
            else LOG("audio: reading %llu KB at %llu failed: %s", (unsigned long long)(kBlock / 1024),
                     (unsigned long long)from, error_.c_str());
            return nullptr;
        }
        error_.clear();
        blocks_.emplace_front(index, std::move(data));
        if (blocks_.size() > kKeep) blocks_.pop_back();
        return &blocks_.front().second;
    }

    std::string url_;
    uint64_t size_;
    HeaderFn headers_;
    std::list<std::pair<uint64_t, std::string>> blocks_;  // most recent first
};

}  // namespace

std::shared_ptr<ByteSource> open_file_source(const std::string& path, std::string& error, uint64_t offset,
                                             uint64_t length) {
    auto f = std::make_shared<FileSource>();
    if (!f->open(path, offset, length)) {
        error = "The audio file could not be opened: " + path;
        return nullptr;
    }
    return f;
}

std::shared_ptr<ByteSource> open_http_source(const std::string& url, uint64_t size, HeaderFn headers) {
    if (!size && !headers) {
        // The size is not known (LibriVox lists none): ask the server.
        lib::HttpRequest head;
        head.method = "HEAD";
        head.url = url;
        head.timeout_seconds = 20;
        const lib::HttpResponse r = lib::http_request(head);
        if (r.ok()) size = r.content_length;
        if (!size) LOG("audio: the size of %s is not known", url.c_str());
    }
    return std::make_shared<HttpSource>(url, size, std::move(headers));
}

bool http_fetch(const std::string& url, const HeaderFn& headers, std::string& out, std::string& error,
                const std::atomic<bool>* cancelled, int patience_ms) {
    const uint64_t until = platform::now_ms() + (uint64_t)patience_ms;
    int failures = 0;
    bool rejected = false, renewed = false;
    for (;;) {
        if (cancelled && *cancelled) {
            error = "Cancelled.";
            return false;
        }
        lib::HttpRequest request;
        request.url = url;
        request.no_auto_auth = true;
        request.timeout_seconds = 20;
        request.max_bytes = 16ull * 1024 * 1024;
        if (headers) request.headers = headers(rejected);
        rejected = false;
        lib::HttpResponse r = lib::http_request(request);
        if (r.ok()) {
            out = std::move(r.body);
            return true;
        }
        error = lib::describe_failure(r);
        if (r.status == 401 && !renewed) {
            rejected = renewed = true;
            continue;
        }
        // 404: a segment the server has not transcoded yet. A failed connection
        // is retried a few times too; anything else is final.
        const bool not_ready = r.status == 404, transient = r.status < 0 || r.status >= 500;
        if (!(not_ready || (transient && ++failures <= 3)) || platform::now_ms() >= until) return false;
        platform::sleep_ms(not_ready ? 400 : 1000);
    }
}

}  // namespace audio
