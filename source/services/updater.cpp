// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/updater.h"

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <vector>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/sha256.h"
#include "core/text.h"
#include "net/http.h"

namespace svc::updater {

namespace {

std::atomic<State> g_state{State::Idle};
std::atomic<bool> g_cancel{false};
std::atomic<uint64_t> g_done{0}, g_total{0};
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_version, g_error, g_url, g_sha;
uint64_t g_size = 0;
pthread_t g_thread;
bool g_joinable = false;
int g_delay = 0;

void fail(const std::string& why) {
    pthread_mutex_lock(&g_lock);
    g_error = why;
    pthread_mutex_unlock(&g_lock);
    LOG("update: %s", why.c_str());
    g_state = State::Failed;
}

std::vector<int> numbers(const std::string& v) {
    std::vector<int> out;
    int n = -1;
    for (char c : v) {
        if (c >= '0' && c <= '9') n = (n < 0 ? 0 : n * 10) + (c - '0');
        else if (n >= 0) {
            out.push_back(n);
            n = -1;
        }
    }
    if (n >= 0) out.push_back(n);
    return out;
}

void* check_main(void*) {
    if (g_delay > 0) usleep((useconds_t)g_delay * 1000);
    lib::HttpRequest r;
    r.url = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
    r.headers.emplace_back("Accept", "application/vnd.github+json");
    r.timeout_seconds = 30;
    const lib::HttpResponse resp = lib::http_request(r);
    // No release yet: this build is the newest there is.
    if (resp.status == 404) {
        LOG("update: no release published yet");
        g_state = State::Current;
        return nullptr;
    }
    if (!resp.ok()) {
        fail(resp.status < 0 ? "Could not reach GitHub." : "GitHub did not answer the update check.");
        return nullptr;
    }
    std::string tag, url, sha;
    uint64_t size = 0;
    if (!parse_release(resp.body, tag, url, size, sha)) {
        fail("The release could not be read.");
        return nullptr;
    }
    LOG("update: latest release %s, this build %s%s", tag.c_str(), current_version().c_str(),
        url.empty() ? ", no .pkg attached" : "");
    if (!newer(tag, current_version())) {
        g_state = State::Current;
        return nullptr;
    }
    if (url.empty()) {
        fail("The new release has no package to download.");
        return nullptr;
    }
    pthread_mutex_lock(&g_lock);
    g_version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    g_url = url;
    g_size = size;
    g_sha = sha;
    pthread_mutex_unlock(&g_lock);
    g_state = State::Available;
    return nullptr;
}

void* download_main(void*) {
    pthread_mutex_lock(&g_lock);
    const std::string url = g_url, sha = g_sha;
    const uint64_t size = g_size;
    pthread_mutex_unlock(&g_lock);
    lib::make_dirs(kFolder);
    const std::string temp = std::string(kFile) + ".new";
    lib::HttpRequest r;
    r.url = url;
    r.timeout_seconds = 60;
    if (url.find("/releases/assets/") != std::string::npos) r.headers.emplace_back("Accept", "application/octet-stream");
    auto progress = [](uint64_t done, uint64_t total) {
        g_done = done;
        if (total) g_total = total;
        return !g_cancel.load();
    };
    lib::HttpResponse resp = lib::http_download(r, temp, progress, true);
    if (!resp.ok() && resp.status < 0 && !g_cancel && !sha.empty()) {
        // GitHub keeps release files on release-assets.githubusercontent.com
        // (a Let's Encrypt certificate, ISRG Root X1), which older console
        // firmware may not know. The file is fetched again without that check;
        // it is accepted only if it matches the SHA-256 api.github.com gave
        // over a checked connection.
        LOG("update: %s; trying again without the certificate check (the file is checked by its SHA-256)",
            resp.error.c_str());
        r.verify_certificate = false;
        resp = lib::http_download(r, temp, progress, true);
    }
    if (g_cancel) {
        lib::remove_file(temp + ".part");
        g_state = State::Available;
        return nullptr;
    }
    if (!resp.ok()) {
        // GitHub serves files from release-assets.githubusercontent.com; say
        // exactly what failed (the console's HTTPS has had trouble there).
        LOG("update: download failed: status %d %s", resp.status, resp.error.c_str());
        fail(lib::describe_failure(resp));
        return nullptr;
    }
    const uint64_t on_disk = lib::file_size(temp);
    if (size && on_disk != size) {
        LOG("update: the file is %llu bytes, the release says %llu", (unsigned long long)on_disk, (unsigned long long)size);
        lib::remove_file(temp);
        fail("The download is the wrong size.");
        return nullptr;
    }
    if (!sha.empty() && !lib::iequals(lib::sha256_file(temp), sha)) {
        lib::remove_file(temp);
        fail("The download did not match its checksum.");
        return nullptr;
    }
    lib::remove_file(kFile);
    if (!lib::rename_file(temp, kFile)) {
        fail("Could not save the package.");
        return nullptr;
    }
    LOG("update: package saved to %s", kFile);
    g_state = State::Downloaded;
    return nullptr;
}

void start(void* (*fn)(void*)) {
    if (g_joinable) {
        pthread_join(g_thread, nullptr);
        g_joinable = false;
    }
    pthread_mutex_lock(&g_lock);
    g_error.clear();
    pthread_mutex_unlock(&g_lock);
    g_joinable = pthread_create(&g_thread, nullptr, fn, nullptr) == 0;
    if (!g_joinable) fail("Could not start.");
}

}  // namespace

std::string current_version() {
#ifdef LIB_VERSION
    return LIB_VERSION;
#else
    return "0";
#endif
}

bool newer(const std::string& release, const std::string& running) {
    const auto a = numbers(release), b = numbers(running);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

bool parse_release(const std::string& json, std::string& tag, std::string& url, uint64_t& size, std::string& sha256) {
    bool ok = false;
    const lib::Json j = lib::Json::parse(json, &ok);
    if (!ok) return false;
    tag = j["tag_name"].str();
    url.clear();
    sha256.clear();
    size = 0;
    for (const lib::Json& a : j["assets"].items()) {
        const std::string name = a["name"].str();
        if (!lib::ends_with(lib::lower(name), ".pkg")) continue;
        // The file through the API ("releases/assets/<id>", asked for as
        // application/octet-stream): its redirect to the file host is small.
        // github.com's own download link answers with ~6 KB of headers, more
        // than the console's HTTP library accepts (0x80431073, "response
        // header too large").
        url = a["url"].str();
        if (url.find("/releases/assets/") == std::string::npos) url = a["browser_download_url"].str();
        size = (uint64_t)a["size"].integer(0);
        // GitHub publishes a digest for each file it stores ("sha256:...").
        const std::string digest = a["digest"].str();
        if (lib::starts_with(digest, "sha256:")) sha256 = digest.substr(7);
        break;
    }
    return !tag.empty();
}

void check(int delay_ms) {
    const State s = g_state;
    if (s == State::Checking || s == State::Downloading) return;
    g_delay = delay_ms;
    g_state = State::Checking;
    start(check_main);
}

void download() {
    if (g_state != State::Available) return;
    g_cancel = false;
    g_done = 0;
    g_total = 0;
    g_state = State::Downloading;
    start(download_main);
}

void cancel() { g_cancel = true; }

State state() { return g_state; }

std::string available_version() {
    pthread_mutex_lock(&g_lock);
    std::string v = g_version;
    pthread_mutex_unlock(&g_lock);
    return v;
}

std::string error() {
    pthread_mutex_lock(&g_lock);
    std::string v = g_error;
    pthread_mutex_unlock(&g_lock);
    return v;
}

void progress(uint64_t& done, uint64_t& total) {
    done = g_done;
    total = g_total ? g_total.load() : g_size;
}

void shutdown() {
    g_cancel = true;
    // A check waiting out its start-up delay, or a request: not waited for.
    if (g_joinable && (g_state == State::Checking || g_state == State::Downloading)) pthread_detach(g_thread);
    else if (g_joinable) pthread_join(g_thread, nullptr);
    g_joinable = false;
}

}  // namespace svc::updater
