// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "gfx/image_cache.h"
#include "gfx/canvas.h"

#include <math.h>

#include <pthread.h>
#include <stdlib.h>

#include <list>
#include <map>
#include <memory>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "core/worker.h"
#include "net/http.h"
#include "platform/platform.h"

namespace gfx {

namespace {

// Decoded originals are bounded to this size; nothing is drawn larger. On the
// console (about 420 MB free) the budget and the number of images in flight
// must stay well clear of exhaustion while covers are extracted from files.
constexpr int kMaxDecode = 1600;
// Covers are never drawn larger than about 700 pixels: kept smaller, many more
// fit in the budget, and a whole shelf of them costs little.
constexpr int kMaxCover = 800;

bool is_cover(const std::string& source) {
    return lib::starts_with(source, "cover:") || lib::starts_with(source, "abs-cover:") ||
           lib::starts_with(source, "find-cover:") ||
           lib::ends_with(source, "/cover.jpg");
}
// Each load opens a whole document or downloads a file. Only a few run at once
// (the rest wait in a queue, not on a worker thread), so a screenful of large
// books cannot open many big files together and exhaust memory.
#ifdef LIB_HOST
// The host can use the console's limits (LIBRARIAN_CONSOLE_LIMITS=1) to measure them.
const bool kConsoleLimits = getenv("LIBRARIAN_CONSOLE_LIMITS") != nullptr;
const size_t kBudget = (kConsoleLimits ? 64ull : 160ull) * 1024 * 1024;
const int kMaxInflight = kConsoleLimits ? 2 : 4;
#else
constexpr size_t kBudget = 64ull * 1024 * 1024;
constexpr int kMaxInflight = 2;
#endif
constexpr size_t kMaxEntries = 400;
int g_inflight = 0;
std::list<std::string> g_pending;  // sources waiting to load

struct Entry {
    ImageState state = ImageState::Loading;
    std::shared_ptr<Image> original;
    // Scaled copies, by "w x h x fit".
    std::map<uint64_t, Image> sized;
    uint64_t used = 0;
    size_t bytes() const {
        size_t n = original ? original->bytes() : 0;
        for (const auto& [k, img] : sized) n += img.bytes();
        return n;
    }
};

std::map<std::string, Entry> g_entries;
uint64_t g_clock = 0;
size_t g_bytes = 0;

// Loaders for sources inside archives; read by workers.
pthread_mutex_t g_loader_lock = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, std::function<bool(std::string&)>> g_loaders;
std::vector<std::pair<std::string, std::function<bool(const std::string&, std::string&)>>> g_prefix_loaders;

void recount() {
    g_bytes = 0;
    for (const auto& [k, e] : g_entries) g_bytes += e.bytes();
}

void evict() {
    while ((g_bytes > kBudget || g_entries.size() > kMaxEntries) && g_entries.size() > 1) {
        auto oldest = g_entries.end();
        for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
            if (it->second.state == ImageState::Loading) continue;
            if (oldest == g_entries.end() || it->second.used < oldest->second.used) oldest = it;
        }
        if (oldest == g_entries.end()) break;
        g_bytes -= oldest->second.bytes();
        g_entries.erase(oldest);
    }
}

bool fetch(const std::string& source, std::string& bytes) {
    {
        pthread_mutex_lock(&g_loader_lock);
        auto found = g_loaders.find(source);
        std::function<bool(std::string&)> loader = found == g_loaders.end() ? nullptr : found->second;
        pthread_mutex_unlock(&g_loader_lock);
        if (loader) return loader(bytes);
        std::function<bool(const std::string&, std::string&)> prefixed;
        pthread_mutex_lock(&g_loader_lock);
        for (const auto& [prefix, fn] : g_prefix_loaders)
            if (lib::starts_with(source, prefix)) prefixed = fn;
        pthread_mutex_unlock(&g_loader_lock);
        if (prefixed) return prefixed(source, bytes);
    }
    if (lib::starts_with(source, "http://") || lib::starts_with(source, "https://")) {
        lib::HttpRequest request;
        request.url = source;
        request.max_bytes = 24 * 1024 * 1024;
        request.timeout_seconds = 15;
        lib::HttpResponse response = lib::http_request(request);
        if (!response.ok()) return false;
        bytes = std::move(response.body);
        return true;
    }
    return lib::read_file(source, bytes, 64 * 1024 * 1024);
}

// Called on the UI thread after a load finishes: start the next queued one.
void pump() {
    while (g_inflight < kMaxInflight && !g_pending.empty()) {
        std::string source = std::move(g_pending.front());
        g_pending.pop_front();
        // The entry may have been evicted or forgotten while it waited.
        auto found = g_entries.find(source);
        if (found == g_entries.end() || found->second.state != ImageState::Loading) continue;
        ++g_inflight;
        lib::run_async(
            [source]() {
                std::string bytes;
                auto image = std::make_shared<Image>();
                const int limit = is_cover(source) ? kMaxCover : kMaxDecode;
                const bool ok = fetch(source, bytes) && decode_image(bytes, *image, limit, limit);
                if (!ok) LOG("image failed: %s", source.substr(0, 160).c_str());
                lib::post([source, image, ok]() {
                    --g_inflight;
                    auto found = g_entries.find(source);
                    if (found != g_entries.end()) {
                        found->second.state = ok ? ImageState::Ready : ImageState::Failed;
                        if (ok) found->second.original = image;
                        recount();
                        evict();
                    }
                    pump();
                });
            },
            0);
    }
}

void start_load(const std::string& source) {
    g_pending.push_back(source);
    pump();
}

Entry& entry(const std::string& source) {
    auto found = g_entries.find(source);
    if (found != g_entries.end()) {
        found->second.used = ++g_clock;
        return found->second;
    }
    Entry& e = g_entries[source];
    e.used = ++g_clock;
    start_load(source);
    return e;
}

}  // namespace

const Image* image_get(const std::string& source, int lw, int lh, Fit fit) {
    if (source.empty() || lw <= 0 || lh <= 0) return nullptr;
    Entry& e = entry(source);
    if (e.state != ImageState::Ready || !e.original) return nullptr;
    // Made at the screen's real resolution (gfx/canvas.h); lw x lh is logical.
    const float density = render_scale();
    const int w = std::max(1, (int)lroundf(lw * density)), h = std::max(1, (int)lroundf(lh * density));
    const uint64_t k = ((uint64_t)w << 33) | ((uint64_t)h << 1) | (fit == Fit::Cover ? 1 : 0);
    auto found = e.sized.find(k);
    if (found != e.sized.end()) return &found->second;
    Image scaled = fit == Fit::Cover ? cover_image(*e.original, w, h) : fit_image(*e.original, w, h);
    scaled.density = density;
    g_bytes += scaled.bytes();
    // Keep a few sizes per image at most.
    if (e.sized.size() >= 4) {
        g_bytes -= e.sized.begin()->second.bytes();
        e.sized.erase(e.sized.begin());
    }
    const Image* result = &(e.sized[k] = std::move(scaled));
    evict();
    // Eviction never removes the most recently used entry.
    return result;
}

ImageState image_state(const std::string& source) {
    auto found = g_entries.find(source);
    return found == g_entries.end() ? ImageState::Loading : found->second.state;
}

void image_natural_size(const std::string& source, int& w, int& h) {
    w = h = 0;
    auto found = g_entries.find(source);
    if (found != g_entries.end() && found->second.original) {
        w = found->second.original->w;
        h = found->second.original->h;
    }
}

void image_register_loader(const std::string& source, std::function<bool(std::string&)> loader) {
    pthread_mutex_lock(&g_loader_lock);
    g_loaders[source] = std::move(loader);
    pthread_mutex_unlock(&g_loader_lock);
}

void image_register_prefix_loader(const std::string& prefix,
                                  std::function<bool(const std::string&, std::string&)> loader) {
    pthread_mutex_lock(&g_loader_lock);
    g_prefix_loaders.emplace_back(prefix, std::move(loader));
    pthread_mutex_unlock(&g_loader_lock);
}

void images_forget(const std::string& prefix) {
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (lib::starts_with(it->first, prefix)) it = g_entries.erase(it);
        else ++it;
    }
    pthread_mutex_lock(&g_loader_lock);
    for (auto it = g_loaders.begin(); it != g_loaders.end();) {
        if (lib::starts_with(it->first, prefix)) it = g_loaders.erase(it);
        else ++it;
    }
    pthread_mutex_unlock(&g_loader_lock);
    recount();
}

void images_retry_failed(const std::string& prefix) {
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->second.state == ImageState::Failed && lib::starts_with(it->first, prefix)) it = g_entries.erase(it);
        else ++it;
    }
}

size_t images_memory() { return g_bytes; }

}  // namespace gfx
