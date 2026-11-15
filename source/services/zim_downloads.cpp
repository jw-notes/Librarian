// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/zim_downloads.h"

#include <pthread.h>

#include <atomic>
#include <deque>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/text.h"
#include "core/worker.h"
#include "net/http.h"
#include "net/url.h"
#include "platform/platform.h"
#include "services/local_files.h"
#include "services/settings.h"
#include "services/zim_shelf.h"

namespace svc {

std::string zim_folder(bool usb) {
    if (!usb) return lib::join_path(platform::data_root(), "zim");
    const std::string drive = first_connected_usb();
    return drive.empty() ? std::string() : lib::join_path(lib::join_path(drive, "Librarian"), "Kiwix");
}

ZimOnDisk zim_on_disk(const ZimBook& book, bool usb) {
    ZimOnDisk d;
    const std::string folder = zim_folder(usb);
    if (folder.empty()) return d;
    d.path = lib::join_path(folder, book.file_name());
    d.complete = lib::file_exists(d.path);
    if (!d.complete && lib::file_exists(d.path + ".part")) d.partial = lib::file_size(d.path + ".part");
    return d;
}

namespace {

struct State {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
    std::deque<ZimJob> jobs;  // the newest last; finished ones stay listed
    int next_id = 1;
    int cancel_id = 0;        // the running job is asked to stop
    bool stopping = false, started = false;
    std::atomic<bool> finished{false};  // the thread has returned
    pthread_t thread{};
    std::atomic<uint64_t> generation{0};
};

State& state() {
    static State s;
    return s;
}

ZimJob* find(State& s, int id) {
    for (auto& j : s.jobs)
        if (j.id == id) return &j;
    return nullptr;
}

void run_one(State& s, int id) {
    pthread_mutex_lock(&s.lock);
    ZimJob* job = find(s, id);
    const std::string url = job->url, path = job->path;
    const bool on_usb = job->on_usb;
    job->state = ZimJob::State::Running;
    pthread_mutex_unlock(&s.lock);
    ++s.generation;

    lib::make_dirs(lib::parent_dir(path));
    LOG("zim: downloading %s to %s", lib::file_name(path).c_str(), path.c_str());
    lib::HttpRequest request;
    request.url = url;
    request.timeout_seconds = 60;
    bool cancelled = false;
    const lib::HttpResponse r = lib::http_download(
        request, path,
        [&](uint64_t done, uint64_t total) {
            pthread_mutex_lock(&s.lock);
            ZimJob* j = find(s, id);
            j->done = done;
            if (total) j->total = total;
            cancelled = s.cancel_id == id;
            const bool stop = cancelled || s.stopping;
            pthread_mutex_unlock(&s.lock);
            ++s.generation;
            return !stop;
        },
        /*resume=*/true);

    pthread_mutex_lock(&s.lock);
    ZimJob* j = find(s, id);
    if (r.ok()) {
        j->state = ZimJob::State::Done;
        j->done = j->total = lib::file_size(path);
    } else if (cancelled) {
        j->state = ZimJob::State::Cancelled;
    } else {
        j->state = ZimJob::State::Failed;
        j->error = s.stopping ? std::string("Stopped when Librarian closed.") : lib::describe_failure(r);
        if (on_usb && j->done >= kFat32Limit - 64 * 1024 * 1024)
            j->error += " A FAT32 drive cannot hold files over 4 GB: format the drive as exFAT.";
    }
    if (s.cancel_id == id) s.cancel_id = 0;
    const ZimJob::State final_state = j->state;
    const std::string error = j->error;
    pthread_mutex_unlock(&s.lock);
    // Cancelled by the user: the part done goes too. Failed or stopped: it
    // stays, and the next start continues it.
    if (final_state == ZimJob::State::Cancelled) lib::remove_file(path + ".part");
    // A finished collection can be read at once.
    if (final_state == ZimJob::State::Done) lib::post([] { zim_shelf().scan_async(); });
    LOG("zim: %s %s %s", lib::file_name(path).c_str(),
        final_state == ZimJob::State::Done ? "downloaded" : final_state == ZimJob::State::Cancelled ? "cancelled" : "failed:",
        error.c_str());
    ++s.generation;
}

void* thread_main(void*) {
    State& s = state();
    for (;;) {
        pthread_mutex_lock(&s.lock);
        int id = 0;
        while (!s.stopping) {
            for (const auto& j : s.jobs)
                if (j.state == ZimJob::State::Queued) {
                    id = j.id;
                    break;
                }
            if (id) break;
            pthread_cond_wait(&s.wake, &s.lock);
        }
        const bool stop = s.stopping;
        pthread_mutex_unlock(&s.lock);
        if (stop) {
            s.finished = true;
            return nullptr;
        }
        run_one(s, id);
    }
}

}  // namespace

bool ZimDownloads::start(const ZimBook& book, bool to_usb, std::string& error) {
    if (book.download.empty()) {
        error = "This collection has no download address.";
        return false;
    }
    const std::string folder = zim_folder(to_usb);
    if (folder.empty()) {
        error = "No USB drive is connected.";
        return false;
    }
    const ZimOnDisk disk = zim_on_disk(book, to_usb);
    if (disk.complete) {
        error = "It is already downloaded: " + disk.path;
        return false;
    }
    if (active(disk.path)) {
        error = "It is already downloading.";
        return false;
    }
    // Free space is known on some storage only (not the console's own).
    const uint64_t free = lib::free_space(lib::dir_exists(folder) ? folder : lib::parent_dir(folder));
    const uint64_t needed = book.size > disk.partial ? book.size - disk.partial : 0;
    if (free && needed && free < needed + 16 * 1024 * 1024) {
        error = "Not enough space: it needs " + lib::format_bytes(needed) + ", and " + lib::format_bytes(free) + " is free.";
        return false;
    }
    State& s = state();
    pthread_mutex_lock(&s.lock);
    ZimJob job;
    job.id = s.next_id++;
    job.title = book.title;
    job.url = book.download;
    job.path = disk.path;
    job.total = book.size;
    job.done = disk.partial;
    job.on_usb = to_usb;
    s.jobs.push_back(job);
    while (s.jobs.size() > 30 && s.jobs.front().state != ZimJob::State::Queued &&
           s.jobs.front().state != ZimJob::State::Running)
        s.jobs.pop_front();
    if (!s.started) {
        s.started = true;
        s.stopping = false;
        s.finished = false;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&s.thread, &attr, thread_main, nullptr) != 0) s.started = false;
        pthread_attr_destroy(&attr);
    }
    const bool running = s.started;
    pthread_cond_signal(&s.wake);
    pthread_mutex_unlock(&s.lock);
    ++s.generation;
    if (!running) {
        error = "The download could not start.";
        return false;
    }
    return true;
}

void ZimDownloads::cancel(int id) {
    State& s = state();
    pthread_mutex_lock(&s.lock);
    if (ZimJob* j = find(s, id)) {
        if (j->state == ZimJob::State::Queued) j->state = ZimJob::State::Cancelled;
        else if (j->state == ZimJob::State::Running) s.cancel_id = id;
    }
    pthread_mutex_unlock(&s.lock);
    ++s.generation;
}

std::vector<ZimJob> ZimDownloads::jobs() const {
    State& s = state();
    pthread_mutex_lock(&s.lock);
    std::vector<ZimJob> out(s.jobs.begin(), s.jobs.end());
    pthread_mutex_unlock(&s.lock);
    return out;
}

bool ZimDownloads::active(const std::string& path) const {
    State& s = state();
    pthread_mutex_lock(&s.lock);
    bool found = false;
    for (const auto& j : s.jobs)
        found = found || (j.path == path && (j.state == ZimJob::State::Queued || j.state == ZimJob::State::Running));
    pthread_mutex_unlock(&s.lock);
    return found;
}

uint64_t ZimDownloads::generation() const { return state().generation.load(); }

void ZimDownloads::stop() {
    State& s = state();
    pthread_mutex_lock(&s.lock);
    const bool started = s.started;
    s.stopping = true;
    pthread_cond_broadcast(&s.wake);
    pthread_mutex_unlock(&s.lock);
    // A transfer notices within a block; one waiting on a silent server could
    // take its whole timeout, so the app does not wait more than a few seconds.
    for (int i = 0; started && !s.finished && i < 300; ++i) platform::sleep_ms(10);
    if (started) {
        if (s.finished) pthread_join(s.thread, nullptr);
        else pthread_detach(s.thread);
    }
    pthread_mutex_lock(&s.lock);
    s.started = false;
    s.stopping = false;
    pthread_mutex_unlock(&s.lock);
}

ZimDownloads& zim_downloads() {
    static ZimDownloads d;
    return d;
}

// ---- the download helper ------------------------------------------------------------

namespace kiwix_helper {

namespace {

std::string base() { return lib::normalise_server_url(settings().kiwix_helper_url); }

lib::HttpResponse call(const std::string& method, const std::string& path, const std::string& body, std::string& error) {
    lib::HttpRequest request;
    request.method = method;
    request.url = base() + path;
    request.body = body;
    request.timeout_seconds = 20;
    request.no_auto_auth = true;
    request.headers.emplace_back("Authorization", "Bearer " + settings().kiwix_helper_key);
    if (!body.empty()) request.headers.emplace_back("Content-Type", "application/json");
    lib::HttpResponse r = lib::http_request(request);
    if (!r.ok()) {
        bool ok = false;
        const lib::Json j = lib::Json::parse(r.body, &ok);
        if (r.status == 401) error = "The helper refused the key: check it in Settings \xE2\x80\xBA Knowledge.";
        else if (ok && !j["error"].str().empty()) error = j["error"].str();
        else error = lib::describe_failure(r);
    }
    return r;
}

}  // namespace

bool configured() { return !base().empty(); }

bool status(Status& out, std::string& error) {
    const lib::HttpResponse r = call("GET", "/api/status", "", error);
    if (!r.ok()) return false;
    bool ok = false;
    const lib::Json j = lib::Json::parse(r.body, &ok);
    if (!ok || j["app"].str() != "librarian-kiwix-helper") {
        error = "That address answers, but not as Librarian's Kiwix download helper.";
        return false;
    }
    out.folder = j["folder"].str();
    out.free_bytes = (uint64_t)j["free_bytes"].integer(0);
    out.updates_library = j["updates_library"].boolean(false);
    return true;
}

bool send(const ZimBook& book, std::string& error) {
    if (book.download.empty()) {
        error = "This collection has no download address.";
        return false;
    }
    lib::Json body = lib::Json::object();
    body.set("url", book.download);
    body.set("title", book.title);
    body.set("size", (int64_t)book.size);
    const lib::HttpResponse r = call("POST", "/api/downloads", body.dump(), error);
    if (r.ok()) LOG("zim: sent %s to the helper", book.file_name().c_str());
    return r.ok();
}

bool parse_jobs(const std::string& json, std::vector<Job>& out) {
    out.clear();
    bool ok = false;
    const lib::Json j = lib::Json::parse(json, &ok);
    if (!ok) return false;
    for (const lib::Json& d : j["downloads"].items()) {
        Job job;
        job.id = d["id"].str();
        job.title = d["title"].str();
        job.file = d["file"].str();
        job.state = d["state"].str();
        job.error = d["error"].str();
        job.done = (uint64_t)d["done"].integer(0);
        job.total = (uint64_t)d["total"].integer(0);
        if (!job.id.empty()) out.push_back(std::move(job));
    }
    return true;
}

bool jobs(std::vector<Job>& out, std::string& error) {
    const lib::HttpResponse r = call("GET", "/api/downloads", "", error);
    if (!r.ok()) return false;
    if (!parse_jobs(r.body, out)) {
        error = "The helper's answer could not be read.";
        return false;
    }
    return true;
}

bool cancel(const std::string& id, std::string& error) {
    return call("DELETE", "/api/downloads/" + lib::url_encode(id), "", error).ok();
}

}  // namespace kiwix_helper
}  // namespace svc
