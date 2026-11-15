// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/audiobookshelf.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>

#include "audio/decoder.h"
#include "core/log.h"
#include "core/text.h"
#include "gfx/image_cache.h"
#include "net/url.h"
#include "platform/platform.h"
#include "services/settings.h"

#ifndef LIB_VERSION
#define LIB_VERSION "dev"
#endif

namespace svc::abs {

namespace {

// Tokens, for the server and account they came from.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// Held while signing in or renewing, so parallel requests do not each sign in
// (the server limits sign-in attempts).
pthread_mutex_t g_sign_in_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_for, g_access, g_refresh, g_user;
// When the access token runs out, on the monotonic clock (0: not known). Taken
// from the token's own lifetime (exp - iat) counted from when it arrived, so a
// console whose date is wrong does not renew it on every request.
uint64_t g_valid_until = 0;
// A sign-in the server refused (wrong password, too many attempts) is not
// tried again by background requests for a minute: the server limits attempts.
std::string g_refused_for;
Status g_refused;
uint64_t g_refused_at = 0;


std::string account_key() {
    const Settings& s = settings();
    return server() + "\n" + s.abs_user + "\n" + s.abs_api_key;
}

std::string bearer() {
    pthread_mutex_lock(&g_lock);
    const std::string t = g_for == account_key() ? g_access : std::string();
    pthread_mutex_unlock(&g_lock);
    return t;
}

void keep(const std::string& access, const std::string& refresh, const std::string& user) {
    pthread_mutex_lock(&g_lock);
    g_for = account_key();
    g_access = access;
    if (!refresh.empty()) g_refresh = refresh;
    if (!user.empty()) g_user = user;
    const int64_t lifetime = token_lifetime(access);
    g_valid_until = lifetime > 0 ? platform::now_ms() + (uint64_t)lifetime * 1000 : 0;
    pthread_mutex_unlock(&g_lock);
}

// True when the current token runs out within `margin_ms` (false if unknown).
bool expiring(uint64_t margin_ms) {
    pthread_mutex_lock(&g_lock);
    const bool soon = g_valid_until && platform::now_ms() + margin_ms >= g_valid_until;
    pthread_mutex_unlock(&g_lock);
    return soon;
}

lib::HttpResponse send(const std::string& method, const std::string& path, const std::string& body,
                       std::vector<std::pair<std::string, std::string>> headers, const std::string& token) {
    lib::HttpRequest r;
    r.url = lib::starts_with(path, "http") ? path : server() + path;
    r.method = method;
    r.body = body;
    r.no_auto_auth = true;  // the Calibre-Web credentials are not for this server
    r.timeout_seconds = 20;
    r.headers = std::move(headers);
    if (!body.empty()) r.headers.emplace_back("Content-Type", "application/json");
    r.headers.emplace_back("Accept", "application/json");
    if (!token.empty()) r.headers.emplace_back("Authorization", "Bearer " + token);
    return lib::http_request(r);
}

Status failure(const lib::HttpResponse& r) {
    Status s;
    s.status = r.status;
    if (r.status == 401) s.error = "Audiobookshelf did not accept the sign-in. Check the user name and password in Settings.";
    else if (r.status == 403) s.error = "This Audiobookshelf account is not allowed to do that.";
    else if (r.status == 404) s.error = "Audiobookshelf has no such item (404).";
    else if (r.status == 429) s.error = "Too many sign-in attempts. Wait a few minutes, then try again.";
    else s.error = lib::describe_failure(r);
    return s;
}

// Signs in with the account (the caller holds g_sign_in_lock).
Status sign_in_locked() {
    const Settings& s = settings();
    if (server().empty()) return {0, "Add the Audiobookshelf address in Settings."};
    if (!s.abs_api_key.empty()) {
        // An API key is a token already: check it and learn the user's name.
        lib::HttpResponse r = send("GET", "/api/me", "", {}, s.abs_api_key);
        if (!r.ok()) {
            Status st = failure(r);
            if (r.status == 401) st.error = "Audiobookshelf did not accept the API key in Settings.";
            return st;
        }
        keep(s.abs_api_key, "", lib::Json::parse(r.body)["username"].str());
        return {r.status, ""};
    }
    if (s.abs_user.empty()) return {0, "Enter your Audiobookshelf user name in Settings."};
    lib::Json body = lib::Json::object();
    body.set("username", s.abs_user);
    body.set("password", s.abs_password);
    LOG("abs: signing in to %s as %s", server().c_str(), s.abs_user.c_str());
    lib::HttpResponse r = send("POST", "/login", body.dump(), {{"x-return-tokens", "true"}}, "");
    if (!r.ok()) {
        // What the server said (never the password: it is not in the answer).
        std::string said = r.error.empty() ? r.body.substr(0, 120) : r.error;
        for (char& c : said)
            if (c == '\n' || c == '\r') c = ' ';
        LOG("abs: sign-in failed (%d): %s", r.status, said.c_str());
        Status st = failure(r);
        if (r.status == 401)
            st.error = "Audiobookshelf did not accept the password for \"" + s.abs_user +
                       "\". Its own log (Settings \xE2\x80\xBA Logs in its web page) says whether the user or the password was wrong.";
        else if (r.status == 400)
            st.error = "Audiobookshelf did not receive the user name and password (400: " + lib::trim(said) + ").";
        return st;
    }
    bool ok = false;
    const lib::Json j = lib::Json::parse(r.body, &ok);
    const lib::Json& user = j["user"];
    // 2.26 and later: a short-lived access token; older servers: one token.
    std::string access = user["accessToken"].str();
    if (access.empty()) access = user["token"].str();
    if (!ok || access.empty()) return {r.status, "The server answered, but not like Audiobookshelf."};
    keep(access, user["refreshToken"].str(), user["username"].str());
    LOG("abs: signed in as %s", user["username"].str().c_str());
    return {r.status, ""};
}

// A refused or expiring token: renew it with the refresh token, or sign in again.
Status renew(const std::string& refused) {
    pthread_mutex_lock(&g_sign_in_lock);
    if (g_refused_for == account_key() && platform::now_ms() - g_refused_at < 60000) {
        const Status st = g_refused;
        pthread_mutex_unlock(&g_sign_in_lock);
        return st;
    }
    // Another request may have renewed it meanwhile.
    const std::string current = bearer();
    if (!current.empty() && current != refused && !expiring(120000)) {
        pthread_mutex_unlock(&g_sign_in_lock);
        return {200, ""};
    }
    Status st;
    pthread_mutex_lock(&g_lock);
    const std::string refresh = g_for == account_key() ? g_refresh : std::string();
    pthread_mutex_unlock(&g_lock);
    if (!refresh.empty() && settings().abs_api_key.empty()) {
        lib::HttpResponse r = send("POST", "/auth/refresh", "", {{"x-refresh-token", refresh}, {"x-return-tokens", "true"}}, "");
        const lib::Json user = lib::Json::parse(r.body)["user"];
        if (r.ok() && !user["accessToken"].str().empty()) {
            keep(user["accessToken"].str(), user["refreshToken"].str(), user["username"].str());
            pthread_mutex_unlock(&g_sign_in_lock);
            return {r.status, ""};
        }
        LOG("abs: token refresh failed (%d); signing in again", r.status);
    }
    st = sign_in_locked();
    if (st.status == 401 || st.status == 429) {
        g_refused_for = account_key();
        g_refused = st;
        g_refused_at = platform::now_ms();
    }
    pthread_mutex_unlock(&g_sign_in_lock);
    return st;
}

// A token that is valid for a while yet, signing in if needed.
std::string token(Status& st) {
    std::string t = bearer();
    if (t.empty() || expiring(120000)) {
        st = renew(t);
        if (!st.ok()) return "";
        t = bearer();
    }
    return t;
}

// A JSON API call, with one retry after renewing a refused token.
Status call(const std::string& method, const std::string& path, const lib::Json* body, lib::Json* out) {
    if (server().empty()) return {0, "Add the Audiobookshelf address in Settings."};
    Status st;
    std::string t = token(st);
    if (t.empty()) return st;
    const std::string payload = body ? body->dump() : std::string();
    lib::HttpResponse r = send(method, path, payload, {}, t);
    if (r.status == 401) {
        st = renew(t);
        if (!st.ok()) return st;
        r = send(method, path, payload, {}, bearer());
    }
    if (!r.ok()) return failure(r);
    if (out) {
        bool ok = false;
        *out = lib::Json::parse(r.body, &ok);
        if (!ok) return {r.status, "The server's answer could not be read."};
    }
    return {r.status, ""};
}

std::string join_names(const lib::Json& list, const char* field) {
    std::string out;
    for (const lib::Json& e : list.items()) {
        const std::string n = field ? e[field].str() : e.str();
        if (!n.empty()) out += (out.empty() ? "" : ", ") + n;
    }
    return out;
}

std::string device_id() {
    // Made once (see ensure in settings_changed) and kept in settings.json.
    const std::string id = settings().abs_device_id;
    return id.empty() ? "librarian-ps4" : id;
}

}  // namespace

std::string server() { return lib::normalise_server_url(settings().abs_url); }
bool configured() { return !server().empty(); }

void settings_changed() {
    pthread_mutex_lock(&g_sign_in_lock);
    g_refused_for.clear();  // new details deserve a try
    pthread_mutex_unlock(&g_sign_in_lock);
    pthread_mutex_lock(&g_lock);
    g_for.clear();
    g_access.clear();
    g_refresh.clear();
    g_user.clear();
    pthread_mutex_unlock(&g_lock);
    Settings& s = settings();
    if (s.abs_device_id.empty()) {
        // A random identifier for this console, so the server's list of
        // sessions shows one device rather than many.
        srand((unsigned)time(nullptr) ^ (unsigned)platform::now_ms());
        char id[40];
        snprintf(id, sizeof id, "librarian-%08x%08x", (unsigned)rand(), (unsigned)rand());
        s.abs_device_id = id;
        settings_save();
    }
}

Status sign_in() {
    // Asked for explicitly (Settings): always tried, and it resets the hold.
    pthread_mutex_lock(&g_sign_in_lock);
    const Status st = sign_in_locked();
    if (st.status == 401 || st.status == 429) {
        g_refused_for = account_key();
        g_refused = st;
        g_refused_at = platform::now_ms();
    } else {
        g_refused_for.clear();
    }
    pthread_mutex_unlock(&g_sign_in_lock);
    return st;
}

bool signed_in() { return !bearer().empty(); }

std::string user_name() {
    pthread_mutex_lock(&g_lock);
    const std::string u = g_for == account_key() ? g_user : std::string();
    pthread_mutex_unlock(&g_lock);
    return u;
}

audio::HeaderFn auth_headers() {
    return [](bool rejected) {
        Status st;
        std::string t = rejected ? (renew(bearer()).ok() ? bearer() : std::string()) : token(st);
        audio::Headers h;
        if (!t.empty()) h.emplace_back("Authorization", "Bearer " + t);
        return h;
    };
}

// ---- parsing -------------------------------------------------------------------------------

Progress parse_progress(const lib::Json& j) {
    Progress p;
    if (!j.is_object()) return p;
    p.found = true;
    p.current_time = j["currentTime"].num();
    p.duration = j["duration"].num();
    p.progress = j["progress"].num();
    p.finished = j["isFinished"].boolean();
    p.updated = j["lastUpdate"].integer();
    return p;
}

Book parse_book(const lib::Json& item) {
    Book b;
    b.id = item["id"].str();
    b.library_id = item["libraryId"].str();
    b.added = item["addedAt"].integer();
    const lib::Json& m = item["media"];
    const lib::Json& md = m["metadata"];
    b.title = md["title"].str();
    b.subtitle = md["subtitle"].str();
    b.author = md["authorName"].str();
    if (b.author.empty()) b.author = join_names(md["authors"], "name");
    b.narrator = md["narratorName"].str();
    if (b.narrator.empty()) b.narrator = join_names(md["narrators"], nullptr);
    b.series = md["seriesName"].str();
    if (b.series.empty() && md["series"].size()) {
        const lib::Json& s = md["series"][0];
        b.series = s["name"].str() + (s["sequence"].str().empty() ? "" : " #" + s["sequence"].str());
    }
    b.description = md["descriptionPlain"].str();
    if (b.description.empty()) b.description = lib::strip_tags(md["description"].str());
    b.publisher = md["publisher"].str();
    b.year = md["publishedYear"].str();
    if (b.year.empty() && md["publishedYear"].type() == lib::Json::Type::Number)
        b.year = std::to_string(md["publishedYear"].integer());
    for (const lib::Json& g : md["genres"].items()) b.genres.push_back(g.str());
    b.duration = m["duration"].num();
    b.size = (uint64_t)(m["size"].integer() ? m["size"].integer() : item["size"].integer());
    for (const lib::Json& c : m["chapters"].items()) b.chapters.push_back({c["start"].num(), c["end"].num(), c["title"].str()});
    b.chapter_count = m.has("numChapters") ? (int)m["numChapters"].integer() : (int)b.chapters.size();
    // The playable files in order with their place in the book ("tracks"), or
    // the audio files when the server sent no tracks.
    const lib::Json& files = m["tracks"].size() ? m["tracks"] : m["audioFiles"];
    double at = 0;
    for (const lib::Json& f : files.items()) {
        if (f["exclude"].boolean(false) || f["invalid"].boolean(false)) continue;
        AudioFile a;
        a.ino = f["ino"].str();
        a.filename = f["metadata"]["filename"].str();
        a.mime = f["mimeType"].str();
        a.codec = f["codec"].str();
        a.size = (uint64_t)f["metadata"]["size"].integer();
        a.duration = f["duration"].num();
        a.start = f.has("startOffset") ? f["startOffset"].num() : at;
        at = a.start + a.duration;
        b.files.push_back(a);
    }
    b.file_count = m.has("numAudioFiles") ? (int)m["numAudioFiles"].integer() : (int)b.files.size();
    if (b.duration <= 0) b.duration = at;
    b.progress = parse_progress(item["userMediaProgress"]);
    return b;
}

bool parse_session(const lib::Json& j, const std::string& base, Session& out) {
    out = Session();
    out.id = j["id"].str();
    out.play_method = (int)j["playMethod"].integer();
    out.current_time = j["currentTime"].num();
    out.duration = j["duration"].num();
    for (const lib::Json& c : j["chapters"].items()) out.chapters.push_back({c["start"].num(), c["end"].num(), c["title"].str()});
    for (const lib::Json& t : j["audioTracks"].items()) {
        audio::Track track;
        const std::string url = t["contentUrl"].str();
        track.location = lib::starts_with(url, "http") ? url : base + url;
        track.mime = t["mimeType"].str();
        track.kind = track.mime == "application/vnd.apple.mpegurl" || url.find(".m3u8") != std::string::npos
                         ? audio::Track::Hls
                         : audio::Track::Http;
        track.size = (uint64_t)t["metadata"]["size"].integer();
        track.start = t["startOffset"].num();
        track.duration = t["duration"].num();
        if (track.kind == audio::Track::Http && !track.size) return false;
        out.tracks.push_back(track);
    }
    return !out.id.empty() && !out.tracks.empty();
}

namespace {
lib::Json jwt_claims(const std::string& jwt) {
    const size_t a = jwt.find('.'), b = a == std::string::npos ? a : jwt.find('.', a + 1);
    if (b == std::string::npos) return lib::Json();
    // Base64url without padding.
    std::string payload;
    uint32_t bits = 0;
    int count = 0;
    for (size_t i = a + 1; i < b; ++i) {
        const char c = jwt[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : -1;
        if (v < 0) continue;
        bits = bits << 6 | (uint32_t)v;
        if ((count += 6) >= 8) {
            count -= 8;
            payload += (char)((bits >> count) & 0xFF);
        }
    }
    return lib::Json::parse(payload);
}
}  // namespace

int64_t token_expiry(const std::string& jwt) { return jwt_claims(jwt)["exp"].integer(); }

int64_t token_lifetime(const std::string& jwt) {
    const lib::Json claims = jwt_claims(jwt);
    const int64_t exp = claims["exp"].integer(), iat = claims["iat"].integer();
    return exp > iat && iat > 0 ? exp - iat : 0;
}

// ---- calls ---------------------------------------------------------------------------------

Status libraries(std::vector<Library>& out) {
    lib::Json j;
    Status st = call("GET", "/api/libraries", nullptr, &j);
    out.clear();
    for (const lib::Json& l : j["libraries"].items())
        out.push_back({l["id"].str(), l["name"].str(), l["mediaType"].str()});
    return st;
}

Status books(const std::string& library, int page, int per_page, const std::string& sort, std::vector<Book>& out,
             int& total) {
    std::string by = "media.metadata.title", desc = "0";
    if (sort == "author") by = "media.metadata.authorName";
    else if (sort == "added") by = "addedAt", desc = "1";
    else if (sort == "duration") by = "media.duration";
    lib::Json j;
    Status st = call("GET", "/api/libraries/" + lib::url_encode(library) + "/items?minified=1&limit=" +
                                std::to_string(per_page) + "&page=" + std::to_string(page) + "&sort=" + by + "&desc=" + desc,
                     nullptr, &j);
    out.clear();
    total = (int)j["total"].integer();
    for (const lib::Json& i : j["results"].items()) out.push_back(parse_book(i));
    return st;
}

Status search(const std::string& library, const std::string& query, std::vector<Book>& out) {
    lib::Json j;
    Status st = call("GET", "/api/libraries/" + lib::url_encode(library) + "/search?limit=40&q=" + lib::url_encode(query),
                     nullptr, &j);
    out.clear();
    for (const lib::Json& r : j["book"].items()) out.push_back(parse_book(r["libraryItem"]));
    return st;
}

Status in_progress(std::vector<Book>& out) {
    lib::Json j;
    Status st = call("GET", "/api/me/items-in-progress?limit=25", nullptr, &j);
    out.clear();
    for (const lib::Json& i : j["libraryItems"].items())
        if (i["mediaType"].str() == "book") out.push_back(parse_book(i));
    return st;
}

Status item(const std::string& id, Book& out) {
    lib::Json j;
    Status st = call("GET", "/api/items/" + lib::url_encode(id) + "?expanded=1&include=progress", nullptr, &j);
    if (st.ok()) out = parse_book(j);
    return st;
}

Status me(std::map<std::string, Progress>& progress, std::vector<Bookmark>& bookmarks) {
    lib::Json j;
    Status st = call("GET", "/api/me", nullptr, &j);
    progress.clear();
    bookmarks.clear();
    for (const lib::Json& p : j["mediaProgress"].items())
        if (p["episodeId"].is_null()) progress[p["libraryItemId"].str()] = parse_progress(p);
    for (const lib::Json& b : j["bookmarks"].items())
        bookmarks.push_back({b["libraryItemId"].str(), b["title"].str(), b["time"].num(), b["createdAt"].integer()});
    return st;
}

Status start_session(const std::string& id, Session& out, bool force_transcode) {
    lib::Json device = lib::Json::object();
    device.set("deviceId", device_id());
    device.set("clientName", "Librarian");
    device.set("clientVersion", LIB_VERSION);
    device.set("manufacturer", "Sony");
    device.set("model", "PlayStation 4");
    lib::Json body = lib::Json::object();
    body.set("deviceInfo", std::move(device));
    body.set("mediaPlayer", "Librarian");
    lib::Json mimes = lib::Json::array();
    for (const char* m : {"audio/mpeg", "audio/mp4", "audio/aac", "audio/flac", "audio/ogg", "audio/opus"}) mimes.push(m);
    body.set("supportedMimeTypes", std::move(mimes));
    body.set("forceDirectPlay", false);
    body.set("forceTranscode", force_transcode);
    lib::Json j;
    Status st = call("POST", "/api/items/" + lib::url_encode(id) + "/play", &body, &j);
    if (!st.ok()) return st;
    if (!parse_session(j, server(), out)) return {st.status, "The server did not say how to play this book."};
    LOG("abs: session %s, %s, %zu track(s)", out.id.substr(0, 8).c_str(), out.play_method == 2 ? "transcoded" : "direct",
        out.tracks.size());
    return st;
}

namespace {
Status session_call(const std::string& what, const std::string& session, double current, double listened, double duration) {
    lib::Json body = lib::Json::object();
    body.set("currentTime", current);
    body.set("timeListened", std::max(0.0, listened));
    body.set("duration", duration);
    return call("POST", "/api/session/" + lib::url_encode(session) + "/" + what, &body, nullptr);
}
}  // namespace

Status sync_session(const std::string& s, double c, double l, double d) { return session_call("sync", s, c, l, d); }
Status close_session(const std::string& s, double c, double l, double d) { return session_call("close", s, c, l, d); }

Status set_progress(const std::string& id, double current_time, double duration, Finished finished) {
    lib::Json body = lib::Json::object();
    body.set("currentTime", current_time);
    body.set("duration", duration);
    body.set("progress", duration > 0 ? std::min(1.0, current_time / duration) : 0.0);
    if (finished != Finished::Leave) body.set("isFinished", finished == Finished::Yes);
    return call("PATCH", "/api/me/progress/" + lib::url_encode(id), &body, nullptr);
}

Status add_bookmark(const std::string& id, double time, const std::string& title) {
    lib::Json body = lib::Json::object();
    body.set("time", (int64_t)time);  // the server keeps bookmarks by whole second
    body.set("title", title);
    return call("POST", "/api/me/item/" + lib::url_encode(id) + "/bookmark", &body, nullptr);
}

Status remove_bookmark(const std::string& id, double time) {
    return call("DELETE", "/api/me/item/" + lib::url_encode(id) + "/bookmark/" + std::to_string((int64_t)time), nullptr,
                nullptr);
}

Status download_file(const std::string& id, const std::string& ino, const std::string& path, const lib::Progress& progress) {
    Status st;
    std::string t = token(st);
    if (t.empty()) return st;
    for (int attempt = 0; attempt < 2; ++attempt) {
        lib::HttpRequest r;
        r.url = server() + "/api/items/" + lib::url_encode(id) + "/file/" + lib::url_encode(ino) + "/download";
        r.no_auto_auth = true;
        r.timeout_seconds = 30;
        r.headers.emplace_back("Authorization", "Bearer " + t);
        lib::HttpResponse resp = lib::http_download(r, path, progress);
        if (resp.ok()) return {resp.status, ""};
        if (resp.status == 401 && attempt == 0) {
            st = renew(t);
            if (!st.ok()) return st;
            t = bearer();
            continue;
        }
        Status f = failure(resp);
        if (resp.status < 0 && !resp.error.empty()) f.error = resp.error;
        return f;
    }
    return st;
}

Status download_cover(const std::string& id, const std::string& path) {
    Status st;
    const std::string t = token(st);
    if (t.empty()) return st;
    lib::HttpRequest r;
    r.url = server() + "/api/items/" + lib::url_encode(id) + "/cover?width=800&format=jpeg";
    r.no_auto_auth = true;
    r.headers.emplace_back("Authorization", "Bearer " + t);
    lib::HttpResponse resp = lib::http_download(r, path, nullptr);
    return resp.ok() ? Status{resp.status, ""} : failure(resp);
}

std::string cover_source(const std::string& id) { return "abs-cover:" + id; }

void register_cover_loader() {
    gfx::image_register_prefix_loader("abs-cover:", [](const std::string& source, std::string& bytes) {
        if (!configured()) return false;
        const std::string id = source.substr(10);
        Status st;
        const std::string t = token(st);
        lib::HttpResponse r = send("GET", "/api/items/" + lib::url_encode(id) + "/cover?width=480&format=jpeg", "", {}, t);
        if (!r.ok()) return false;
        bytes = std::move(r.body);
        return true;
    });
}

Status test_connection(std::string& summary) {
    // Is this Audiobookshelf at all? /status needs no sign-in.
    lib::HttpResponse r = send("GET", "/status", "", {}, "");
    if (!r.ok()) return failure(r);
    const lib::Json status = lib::Json::parse(r.body);
    if (status["app"].str() != "audiobookshelf") return {r.status, "The server at this address is not Audiobookshelf."};
    if (!status["isInit"].boolean(true)) return {r.status, "Audiobookshelf has not been set up yet: open it in a browser first."};
    const std::string version = status["serverVersion"].str();
    Status st = sign_in();
    if (!st.ok()) return st;
    std::vector<Library> libs;
    st = libraries(libs);
    if (!st.ok()) return st;
    int books_total = 0, book_libraries = 0;
    for (const Library& l : libs) {
        if (l.media_type != "book") continue;
        ++book_libraries;
        std::vector<Book> page;
        int total = 0;
        if (books(l.id, 0, 1, "title", page, total).ok()) books_total += total;
    }
    summary = "Audiobookshelf " + version + ", signed in as " + user_name() + ". " + std::to_string(book_libraries) +
              (book_libraries == 1 ? " library, " : " libraries, ") + std::to_string(books_total) +
              (books_total == 1 ? " audiobook." : " audiobooks.");
    if (!book_libraries) st.error = "Signed in, but this account sees no audiobook libraries (podcasts are not supported).";
    return st;
}

}  // namespace svc::abs
