// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Random-access bytes for the audio decoders: a local file, or a file on a
// server read in blocks with HTTP Range requests. A few recent blocks are kept,
// so the demuxers' small reads do not each become a request, and memory stays
// bounded however long the book is.
#include <stdint.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace audio {

using Headers = std::vector<std::pair<std::string, std::string>>;
// Headers for each request, asked for at request time (an access token may
// have been refreshed since the source was opened). `rejected`: the last ones
// were refused (401), so the provider should renew its token; the request is
// then tried once more.
using HeaderFn = std::function<Headers(bool rejected)>;

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t size() const = 0;
    // Reads up to `n` bytes at `offset`. Returns the count; fewer than asked
    // only at the end of the data or on an error (then error() says why).
    virtual size_t read_at(uint64_t offset, void* out, size_t n) = 0;
    const std::string& error() const { return error_; }
    // Makes reads in progress (and later ones) give up: the player is closing.
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }

protected:
    std::string error_;
    std::atomic<bool> cancelled_{false};
};

// `length` > 0: only `length` bytes from `offset` (a file stored inside an archive).
std::shared_ptr<ByteSource> open_file_source(const std::string& path, std::string& error, uint64_t offset = 0,
                                             uint64_t length = 0);
// `size` must be known (Audiobookshelf reports every file's size).
std::shared_ptr<ByteSource> open_http_source(const std::string& url, uint64_t size, HeaderFn headers);

// Fetches a whole (small) resource, retrying while the server says it is not
// ready yet (an HLS segment still being transcoded answers 404). Gives up after
// `patience_ms`, on another failure, or when `cancelled` becomes true.
bool http_fetch(const std::string& url, const HeaderFn& headers, std::string& out, std::string& error,
                const std::atomic<bool>* cancelled, int patience_ms = 20000);

// Sequential big-endian reading on top of a ByteSource, for the parsers.
class Reader {
public:
    explicit Reader(ByteSource& source) : source_(source) {}
    uint64_t tell() const { return pos_; }
    uint64_t size() const { return source_.size(); }
    void seek(uint64_t pos) { pos_ = pos; }
    bool skip(uint64_t n) {
        pos_ += n;
        return pos_ <= size();
    }
    bool read(void* out, size_t n) {
        const size_t got = source_.read_at(pos_, out, n);
        pos_ += got;
        if (got != n) ok_ = false;
        return got == n;
    }
    uint8_t u8() {
        uint8_t b[1] = {0};
        read(b, 1);
        return b[0];
    }
    uint16_t u16() {
        uint8_t b[2] = {0};
        read(b, 2);
        return (uint16_t)(b[0] << 8 | b[1]);
    }
    uint32_t u24() {
        uint8_t b[3] = {0};
        read(b, 3);
        return (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
    }
    uint32_t u32() {
        uint8_t b[4] = {0};
        read(b, 4);
        return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    }
    uint64_t u64() {
        const uint64_t hi = u32();
        return hi << 32 | u32();
    }
    bool ok() const { return ok_; }

private:
    ByteSource& source_;
    uint64_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace audio
