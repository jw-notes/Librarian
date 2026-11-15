// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/calibre_session.h"

#include <pthread.h>

#include <map>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "net/http.h"
#include "net/url.h"
#include "services/calibre.h"
#include "services/settings.h"

namespace svc::calibre {

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, std::string> g_cookies;  // name -> value
std::string g_cookie_origin;                   // which server the cookies belong to

// Stores the name=value part of each Set-Cookie line.
void absorb(const std::vector<std::string>& set_cookies) {
    for (const std::string& c : set_cookies) {
        const size_t semi = c.find(';');
        const std::string pair = c.substr(0, semi);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = lib::trim(pair.substr(0, eq)), value = lib::trim(pair.substr(eq + 1));
        if (name.empty()) continue;
        // An empty value (or a deletion) clears the cookie.
        if (value.empty() || value == "\"\"") g_cookies.erase(name);
        else g_cookies[name] = value;
    }
}

std::string cookie_header() {
    std::string out;
    for (const auto& [name, value] : g_cookies) out += (out.empty() ? "" : "; ") + name + "=" + value;
    return out;
}

// The hidden csrf_token of the first form in a page.
std::string extract_csrf(const std::string& html) {
    size_t at = html.find("name=\"csrf_token\"");
    if (at == std::string::npos) return "";
    const size_t v = html.find("value=\"", at);
    if (v == std::string::npos) return "";
    const size_t start = v + 7, end = html.find('"', start);
    return end == std::string::npos ? "" : html.substr(start, end - start);
}

// A book's MIME type by extension; browsers send this, and some servers
// validate the declared type of the upload part.
std::string book_mime(const std::string& ext) {
    if (ext == "epub") return "application/epub+zip";
    if (ext == "pdf") return "application/pdf";
    if (ext == "mobi" || ext == "azw" || ext == "prc") return "application/x-mobipocket-ebook";
    if (ext == "azw3") return "application/x-mobi8-ebook";
    if (ext == "fb2") return "application/x-fictionbook+xml";
    if (ext == "cbz") return "application/vnd.comicbook+zip";
    if (ext == "cbr") return "application/vnd.comicbook-rar";
    if (ext == "cb7") return "application/x-cb7";
    if (ext == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == "odt") return "application/vnd.oasis.opendocument.text";
    if (ext == "rtf") return "application/rtf";
    if (ext == "txt") return "text/plain";
    return "application/octet-stream";
}

bool have_session() { return g_cookies.count("session") && g_cookie_origin == server(); }

// A request that carries the session cookie and never the Basic credentials.
lib::HttpRequest session_request(const std::string& url) {
    lib::HttpRequest r;
    r.url = url;
    r.no_auto_auth = true;
    r.follow_redirects = false;  // login success is a 302 we want to see
    const std::string cookies = cookie_header();
    if (!cookies.empty()) r.headers.emplace_back("Cookie", cookies);
    return r;
}

SessionStatus do_login(const std::string& user, const std::string& password) {
    const std::string srv = server();
    if (srv.empty()) return {LoginResult::NoServer, "No Calibre-Web address is set."};
    if (g_cookie_origin != srv) {
        g_cookies.clear();
        g_cookie_origin = srv;
    }
    // 1. GET the login page: a CSRF token and the initial session cookie.
    lib::HttpResponse page = lib::http_request(session_request(srv + "/login"));
    if (page.status < 0) return {LoginResult::NoServer, lib::describe_failure(page)};
    absorb(page.set_cookies);
    const std::string token = extract_csrf(page.body);
    if (page.body.find("Calibre-Web") == std::string::npos && page.body.find("csrf_token") == std::string::npos)
        return {LoginResult::NotCalibre, "This address is not a Calibre-Web sign-in page."};
    if (token.empty()) return {LoginResult::NotCalibre, "The sign-in page could not be read."};

    // 2. POST the credentials. Calibre-Web needs the `next` field, or it answers 400.
    lib::HttpRequest post = session_request(srv + "/login");
    post.method = "POST";
    post.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    post.body = "csrf_token=" + lib::url_encode(token) + "&next=%2F&username=" + lib::url_encode(user) + "&password=" +
                lib::url_encode(password) + "&remember_me=on&submit=";
    lib::HttpResponse r = lib::http_request(post);
    absorb(r.set_cookies);
    if (r.status == 429) return {LoginResult::RateLimited, "Too many sign-in attempts. Wait a minute and try again."};
    // Success: Calibre-Web redirects (302) to `next`. A re-rendered 200/401
    // login page means the credentials were refused.
    if (r.status == 302 || r.status == 303) {
        if (!g_cookies.count("session")) return {LoginResult::WrongCredentials, "The server did not start a session."};
        LOG("calibre: signed in as %s", user.c_str());
        return {LoginResult::Ok, "Signed in."};
    }
    if (r.status == 400) return {LoginResult::WrongCredentials, "The sign-in was rejected (the session may have expired; try again)."};
    return {LoginResult::WrongCredentials, "The user name or password was not accepted."};
}

}  // namespace

SessionStatus login(const std::string& user, const std::string& password) {
    pthread_mutex_lock(&g_lock);
    SessionStatus s = do_login(user, password);
    pthread_mutex_unlock(&g_lock);
    return s;
}

SessionStatus login_from_settings() {
    const Settings& s = settings();
    if (s.calibre_user.empty()) return {LoginResult::WrongCredentials, "Set a Calibre-Web user name and password in Settings first."};
    return login(s.calibre_user, s.calibre_password);
}

bool upload_book(const std::string& path, std::string& message) {
    const std::string srv = server();
    if (srv.empty()) {
        message = "No Calibre-Web address is set.";
        return false;
    }
    std::string data;
    if (!lib::read_file(path, data, 512ull * 1024 * 1024)) {
        message = "The file could not be read.";
        return false;
    }

    pthread_mutex_lock(&g_lock);
    // Sign in if there is no session yet.
    if (!have_session()) {
        const SessionStatus s = do_login(settings().calibre_user, settings().calibre_password);
        if (!s.ok()) {
            pthread_mutex_unlock(&g_lock);
            message = s.message;
            return false;
        }
    }

    // A CSRF token tied to the signed-in session (from any page with a form).
    auto fresh_token = [&]() -> std::string {
        lib::HttpResponse home = lib::http_request(session_request(srv + "/"));
        absorb(home.set_cookies);
        return extract_csrf(home.body);
    };

    const std::string name = lib::safe_file_name(lib::file_stem(path)) + "." + lib::file_extension(path);
    auto attempt = [&](const std::string& token) {
        const std::string boundary = "----LibrarianUpload8f2a1c";
        std::string body;
        body.reserve(data.size() + 512);
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"csrf_token\"\r\n\r\n" + token + "\r\n";
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"btn-upload\"; filename=\"" + name +
                "\"\r\nContent-Type: " + book_mime(lib::file_extension(path)) + "\r\n\r\n";
        body += data;
        body += "\r\n--" + boundary + "--\r\n";
        lib::HttpRequest up = session_request(srv + "/upload");
        up.method = "POST";
        up.timeout_seconds = 120;
        up.body = std::move(body);
        up.max_bytes = 4ull * 1024 * 1024;  // the JSON reply is small
        up.headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
        lib::HttpResponse r = lib::http_request(up);
        absorb(r.set_cookies);
        return r;
    };

    std::string token = fresh_token();
    lib::HttpResponse r = token.empty() ? lib::HttpResponse{} : attempt(token);
    // A redirect to /login means the session lapsed: sign in again and retry once.
    const bool lapsed = r.status == 302 || r.status == 303;  // a redirect from /upload means not signed in
    if (token.empty() || lapsed || r.status == 401 || r.status == 403) {
        const SessionStatus s = do_login(settings().calibre_user, settings().calibre_password);
        if (!s.ok()) {
            pthread_mutex_unlock(&g_lock);
            message = s.message;
            return false;
        }
        token = fresh_token();
        if (!token.empty()) r = attempt(token);
    }
    pthread_mutex_unlock(&g_lock);

    if (r.status == 200) {
        message = "Uploaded to Calibre-Web.";
        LOG("calibre: uploaded %s", name.c_str());
        return true;
    }
    if (r.status == 403) {
        message = "Your account cannot upload books. Ask the server's admin to allow uploads for it.";
        return false;
    }
    if (r.status == 422 || r.status == 400) {
        message = "Calibre-Web refused the file (the format may not be allowed for upload on the server).";
        return false;
    }
    message = r.status > 0 ? "Upload failed (the server answered " + std::to_string(r.status) + ")." : lib::describe_failure(r);
    return false;
}

void forget_session() {
    pthread_mutex_lock(&g_lock);
    g_cookies.clear();
    g_cookie_origin.clear();
    pthread_mutex_unlock(&g_lock);
}

}  // namespace svc::calibre
