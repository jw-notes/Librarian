// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// HTTP client. Redirects are followed here (not by the console's library) so
// the final address of a page is known: Kiwix articles use links relative to
// it. Basic-auth credentials are only ever sent to the origin they belong to.
#include <stdint.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lib {

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string username, password;  // HTTP Basic, for `auth_origin` only
    std::string auth_origin;         // defaults to the origin of `url`
    std::vector<std::pair<std::string, std::string>> headers;
    uint64_t max_bytes = 48ull * 1024 * 1024;
    int timeout_seconds = 20;
    int max_redirects = 8;
    bool follow_redirects = true;  // false: return a 3xx as-is (for login)
    // Do not attach the per-origin stored Basic credentials (used for session/
    // cookie requests, which carry their own auth and must not trip limiters).
    bool no_auto_auth = false;
};

struct HttpResponse {
    int status = -1;  // -1: no response (connection, DNS, TLS failure)
    std::string body;
    std::string final_url;
    std::string content_type;
    std::string error;  // human-readable, for the UI
    std::vector<std::string> set_cookies;  // raw Set-Cookie header values
    uint64_t content_length = 0;           // as the server gave it (0: not given)

    bool ok() const { return status >= 200 && status < 300; }
};

// Called with each chunk of the body; return false to stop the transfer.
using BodySink = std::function<bool(const char* data, size_t size)>;
// Called with bytes so far and the total (0 if the server does not say).
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

bool http_init();
void http_shutdown();
// Credentials sent (HTTP Basic) to every request for this origin, unless the
// request carries its own. Set from Settings; an empty user removes them.
void http_set_credentials(const std::string& origin_url, const std::string& user, const std::string& password);
// Accept self-signed certificates (a setting: many home servers use them).
void http_set_allow_insecure(bool allow);

HttpResponse http_request(const HttpRequest& request);
// Streams the body to `path` (via `path`.part, renamed on success). With
// `resume`, a .part file left by an earlier attempt is continued (HTTP Range)
// when the server allows it, and kept when this attempt fails or is cancelled.
HttpResponse http_download(const HttpRequest& request, const std::string& path, const Progress& progress,
                           bool resume = false);
// Explains a failed response in a sentence fit for an error screen.
std::string describe_failure(const HttpResponse& response);

// ---- implemented by the platform backend (http_ps4.cpp / http_host.cpp) ----
// Performs one request without following redirects. `sink` receives the body
// of any response that is not a 3xx; before it, `head` (optional) sees the
// status and headers, and returning false skips the body.
struct HttpRawResult {
    int status = -1;
    std::string location, content_type, error;
    uint64_t content_length = 0;
    std::vector<std::string> set_cookies;
};
using HeadHook = std::function<bool(const HttpRawResult& head)>;
HttpRawResult http_perform(const std::string& method, const std::string& url, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& headers, int timeout_seconds,
                           const BodySink& sink, const HeadHook& head = nullptr);

// Requests to `origin` (e.g. "http://zim.librarian") are answered in-process
// by `handler` instead of the network: Librarian's own ZIM files are served
// the way kiwix-serve serves its collections. The handler fills the status,
// type and Location of `head` and the body; it runs on the calling thread.
using LocalHandler = std::function<void(const std::string& url, HttpRawResult& head, std::string& body)>;
void http_register_local(const std::string& origin, LocalHandler handler);

}  // namespace lib
