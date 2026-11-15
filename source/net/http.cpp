// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "net/http.h"

#include <pthread.h>
#include <stdio.h>

#include <map>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "net/url.h"

namespace lib {

namespace {

pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, std::pair<std::string, std::string>> g_credentials;  // origin -> user, password
std::map<std::string, LocalHandler> g_local;  // origin -> handler (set at startup, then only read)

std::string origin_of(const std::string& url) {
    Url u;
    return parse_url(url, u) ? lower(u.origin()) : std::string();
}

// Logs the address without its query string, which can hold search terms.
std::string log_target(const std::string& url) {
    Url u;
    if (!parse_url(url, u)) return "(invalid)";
    return u.origin() + url_path_only(u.path).substr(0, 120);
}

thread_local bool t_skip_verification = false;


HttpResponse perform(const HttpRequest& request, const BodySink& sink, uint64_t* total_out,
                     const HeadHook& head = nullptr) {
    HttpResponse response;
    std::string url = request.url;
    const std::string auth_origin = request.auth_origin.empty() ? url : request.auth_origin;
    std::string method = request.method;
    std::string body = request.body;
    for (int hop = 0; hop <= request.max_redirects; ++hop) {
        Url parsed;
        if (!parse_url(url, parsed)) {
            response.error = "The address is not a valid http:// or https:// URL.";
            return response;
        }
        auto headers = request.headers;
        headers.emplace_back("User-Agent", "Librarian-PS4/1.0");
        headers.emplace_back("Accept-Encoding", "identity");
        if (!request.username.empty()) {
            if (same_origin(url, auth_origin))
                headers.emplace_back("Authorization", "Basic " + base64_encode(request.username + ":" + request.password));
        } else if (!request.no_auto_auth) {
            pthread_mutex_lock(&g_auth_lock);
            auto found = g_credentials.find(origin_of(url));
            if (found != g_credentials.end())
                headers.emplace_back("Authorization",
                                     "Basic " + base64_encode(found->second.first + ":" + found->second.second));
            pthread_mutex_unlock(&g_auth_lock);
        }

        HttpRawResult raw;
        auto local = g_local.find(origin_of(url));
        if (local != g_local.end()) {
            // Served in-process: not logged (a page and its pictures are many requests).
            std::string data;
            local->second(url, raw, data);
            raw.content_length = data.size();
            const bool redirect = raw.status >= 300 && raw.status < 400 && !raw.location.empty();
            if (!redirect && method != "HEAD" && (!head || head(raw)) && !data.empty()) sink(data.data(), data.size());
        } else {
            LOGD("HTTP %s %s", method.c_str(), log_target(url).c_str());
            t_skip_verification = !request.verify_certificate;
            raw = http_perform(method, url, body, headers, request.timeout_seconds, sink, head);
            t_skip_verification = false;
        }
        response.status = raw.status;
        response.content_type = raw.content_type;
        response.error = raw.error;
        response.final_url = url;
        for (auto& c : raw.set_cookies) response.set_cookies.push_back(std::move(c));
        if (total_out) *total_out = raw.content_length;
        response.content_length = raw.content_length;
        if (local == g_local.end()) {
            // A failure is always worth a line (with what was asked, which
            // the detail line above only gives in debug logging).
            if (raw.status < 0 || raw.status >= 400)
                LOG("HTTP %s %s: status %d %s", method.c_str(), log_target(url).c_str(), raw.status, raw.error.c_str());
            else
                LOGD("HTTP status %d %s", raw.status, raw.error.c_str());
        }
        if (request.follow_redirects &&
            (raw.status == 301 || raw.status == 302 || raw.status == 303 || raw.status == 307 || raw.status == 308) &&
            !raw.location.empty()) {
            url = resolve_url(url, raw.location);
            if (raw.status == 303 || ((raw.status == 301 || raw.status == 302) && method == "POST")) {
                method = "GET";
                body.clear();
            }
            continue;
        }
        return response;
    }
    response.error = "Too many redirects.";
    response.status = -1;
    return response;
}

}  // namespace

void http_register_local(const std::string& origin, LocalHandler handler) {
    g_local[origin_of(origin)] = std::move(handler);
}

void http_set_credentials(const std::string& origin_url, const std::string& user, const std::string& password) {
    const std::string origin = origin_of(origin_url);
    if (origin.empty()) return;
    pthread_mutex_lock(&g_auth_lock);
    if (user.empty()) g_credentials.erase(origin);
    else g_credentials[origin] = {user, password};
    pthread_mutex_unlock(&g_auth_lock);
}

HttpResponse http_request(const HttpRequest& request) {
    std::string body;
    bool too_big = false;
    HttpResponse response = perform(request, [&](const char* data, size_t size) {
        if (body.size() + size > request.max_bytes) {
            too_big = true;
            return false;
        }
        body.append(data, size);
        return true;
    }, nullptr);
    if (too_big) {
        response.status = -1;
        response.error = "The response is larger than " + format_bytes(request.max_bytes) + ".";
        return response;
    }
    response.body = std::move(body);
    return response;
}

HttpResponse http_download(const HttpRequest& request, const std::string& path, const Progress& progress,
                           bool resume) {
    const std::string temp = path + ".part";
    const uint64_t have = resume && file_exists(temp) ? file_size(temp) : 0;
    HttpRequest req = request;
    if (have > 0) req.headers.emplace_back("Range", "bytes=" + std::to_string(have) + "-");
    FILE* out = nullptr;
    uint64_t done = 0, total = 0;
    bool write_failed = false, cancelled = false, open_failed = false;
    // The file is opened once the answer is known: appended to when the
    // server sends the rest (206), started again when it sends everything.
    auto head = [&](const HttpRawResult& raw) {
        if (raw.status < 200 || raw.status >= 300) return true;  // an error page: read, not kept
        if (raw.status == 206 && have > 0) {
            out = open_file(temp, "ab");
            done = have;
            total = raw.content_length ? have + raw.content_length : 0;
        } else {
            out = open_file(temp, "wb");
            done = 0;
            total = raw.content_length;
        }
        if (!out) open_failed = true;
        return out != nullptr;
    };
    HttpResponse response = perform(req, [&](const char* data, size_t size) {
        if (!out) return true;
        if (fwrite(data, 1, size, out) != size) {
            write_failed = true;
            return false;
        }
        done += size;
        if (progress && !progress(done, total)) {
            cancelled = true;
            return false;
        }
        return true;
    }, nullptr, head);
    const bool closed = !out || fclose(out) == 0;
    if (open_failed) {
        response.status = -1;
        response.error = "Could not create " + path + ".";
    } else if (write_failed || !closed) {
        response.status = -1;
        response.error = "Could not write the file (is the storage full?).";
    } else if (cancelled) {
        response.status = -1;
        response.error = "Cancelled.";
    } else if (response.ok() && total && done != total) {
        response.status = -1;
        response.error = "The download ended early.";
    }
    if (response.ok()) {
        if (!rename_file(temp, path)) {
            response.status = -1;
            response.error = "Could not save " + path + ".";
        }
    }
    if (!response.ok() && (!resume || open_failed)) remove_file(temp);
    return response;
}

std::string describe_failure(const HttpResponse& r) {
    if (r.status == 401) return "The server asked for a user name and password, or rejected the ones in Settings.";
    if (r.status == 403) return "The server refused access (403). Check the account's permissions.";
    if (r.status == 404) return "The server has no such page (404).";
    if (r.status >= 500) return "The server had an internal error (" + std::to_string(r.status) + ").";
    if (r.status > 0 && !r.ok()) return "The server answered with status " + std::to_string(r.status) + ".";
    if (!r.error.empty()) return r.error;
    return "No response from the server. Check the address and the network connection.";
}

bool http_skip_verification() { return t_skip_verification; }

}  // namespace lib
