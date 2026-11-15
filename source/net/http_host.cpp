// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Host HTTP backend for development and tests: HTTP/1.1 over plain sockets.
// HTTPS is not supported here; the console build uses the system's TLS.
#include <ctype.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/text.h"
#include "net/http.h"
#include "net/url.h"

namespace lib {

bool http_init() { return true; }
void http_shutdown() {}
void http_set_allow_insecure(bool) {}

namespace {

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) close(fd_);
    }
    int fd() const { return fd_; }

    // Buffered reads.
    int get() {
        if (pos_ == len_) {
            const ssize_t n = recv(fd_, buffer_, sizeof buffer_, 0);
            if (n <= 0) return -1;
            pos_ = 0;
            len_ = (size_t)n;
        }
        return (unsigned char)buffer_[pos_++];
    }
    bool line(std::string& out) {
        out.clear();
        for (int c; (c = get()) >= 0;) {
            if (c == '\n') {
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            out += (char)c;
            if (out.size() > 16384) return false;
        }
        return false;
    }
    // Reads up to `max` bytes, using the buffer first.
    ssize_t read(char* dst, size_t max) {
        if (pos_ < len_) {
            const size_t n = std::min(max, len_ - pos_);
            memcpy(dst, buffer_ + pos_, n);
            pos_ += n;
            return (ssize_t)n;
        }
        return recv(fd_, dst, max, 0);
    }

private:
    int fd_;
    char buffer_[16384];
    size_t pos_ = 0, len_ = 0;
};

int connect_to(const Url& url, int timeout_seconds, std::string& error) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found) != 0 || !found) {
        error = "Could not find the server " + url.host + ".";
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = found; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{timeout_seconds, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(found);
    if (fd < 0) error = "Could not connect to " + url.host + ":" + std::to_string(url.port) + ".";
    return fd;
}

}  // namespace

HttpRawResult http_perform(const std::string& method, const std::string& url_text, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& headers, int timeout_seconds,
                           const BodySink& sink, const HeadHook& head) {
    HttpRawResult result;
    Url url;
    if (!parse_url(url_text, url)) {
        result.error = "Invalid address.";
        return result;
    }
    if (url.scheme == "https") {
        result.error = "HTTPS is not available in the host build.";
        return result;
    }
    Socket socket(connect_to(url, timeout_seconds, result.error));
    if (socket.fd() < 0) return result;

    std::string request = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                          (url.port != 80 ? ":" + std::to_string(url.port) : "") + "\r\nConnection: close\r\n";
    for (const auto& [k, v] : headers) request += k + ": " + v + "\r\n";
    if (!body.empty()) request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    // Send all of it: one send() can transmit less than the whole buffer for a
    // large body (an upload), which would truncate the request.
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = send(socket.fd(), request.data() + sent, request.size() - sent, 0);
        if (n <= 0) {
            result.error = "Could not send the request.";
            return result;
        }
        sent += (size_t)n;
    }

    std::string line;
    if (!socket.line(line) || !starts_with(line, "HTTP/")) {
        result.error = "The server did not answer with HTTP.";
        return result;
    }
    const size_t space = line.find(' ');
    result.status = space == std::string::npos ? -1 : atoi(line.c_str() + space + 1);
    bool chunked = false;
    int64_t length = -1;
    while (socket.line(line) && !line.empty()) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "location") result.location = value;
        else if (key == "set-cookie") result.set_cookies.push_back(value);
        else if (key == "content-type") result.content_type = value;
        else if (key == "content-length") length = atoll(value.c_str());
        else if (key == "transfer-encoding") chunked = icontains(value, "chunked");
    }
    if (length > 0) result.content_length = (uint64_t)length;
    const bool redirect = result.status >= 300 && result.status < 400 && !result.location.empty();
    if (method == "HEAD" || redirect || result.status == 204 || result.status == 304) return result;
    if (head && !head(result)) return result;

    char buffer[65536];
    auto deliver = [&](uint64_t count) -> bool {
        while (count > 0) {
            const ssize_t n = socket.read(buffer, std::min<uint64_t>(count, sizeof buffer));
            if (n <= 0) {
                result.error = "The connection closed early.";
                result.status = -1;
                return false;
            }
            if (!sink(buffer, (size_t)n)) return false;
            count -= (uint64_t)n;
        }
        return true;
    };
    if (chunked) {
        for (;;) {
            if (!socket.line(line)) break;
            const uint64_t size = strtoull(line.c_str(), nullptr, 16);
            if (size == 0) break;
            if (!deliver(size)) return result;
            socket.line(line);  // CRLF after the chunk
        }
    } else if (length >= 0) {
        deliver((uint64_t)length);
    } else {
        for (ssize_t n; (n = socket.read(buffer, sizeof buffer)) > 0;)
            if (!sink(buffer, (size_t)n)) break;
    }
    return result;
}

}  // namespace lib
