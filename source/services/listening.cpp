// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/listening.h"

#include "services/librivox.h"

#include <math.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "audio/decoder.h"
#include "audio/player.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "doc/covers.h"
#include "core/text.h"
#include "core/worker.h"
#include "platform/platform.h"
#include "services/local_files.h"
#include "services/settings.h"

namespace svc {

int64_t epoch_ms() { return (int64_t)time(nullptr) * 1000; }

std::string format_duration(double seconds) {
    const int64_t s = (int64_t)std::max(0.0, seconds);
    char out[32];
    if (s >= 3600) snprintf(out, sizeof out, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else snprintf(out, sizeof out, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return out;
}

// ---- downloads -----------------------------------------------------------------------------

namespace {

const char* kManifest = "librarian-audiobook.json";

lib::Json chapters_json(const std::vector<abs::Chapter>& chapters) {
    lib::Json list = lib::Json::array();
    for (const auto& c : chapters) {
        lib::Json j = lib::Json::object();
        j.set("start", c.start);
        j.set("end", c.end);
        j.set("title", c.title);
        list.push(std::move(j));
    }
    return list;
}

bool write_manifest(const std::string& dir, const abs::Book& b, bool complete, bool local = false,
                    const std::string& cover_file = "") {
    lib::Json j = lib::Json::object();
    j.set("version", 1);
    j.set("server", local ? std::string() : librivox::is_librivox(b.id) ? std::string("https://librivox.org") : abs::server());
    if (local) j.set("local", true);
    if (!cover_file.empty()) j.set("cover_file", cover_file);
    j.set("item_id", b.id);
    j.set("title", b.title);
    j.set("subtitle", b.subtitle);
    j.set("author", b.author);
    j.set("narrator", b.narrator);
    j.set("series", b.series);
    j.set("description", b.description);
    j.set("year", b.year);
    j.set("duration", b.duration);
    j.set("chapters", chapters_json(b.chapters));
    lib::Json files = lib::Json::array();
    for (const auto& f : b.files) {
        lib::Json e = lib::Json::object();
        e.set("name", f.filename);
        e.set("mime", f.mime);
        e.set("start", f.start);
        e.set("duration", f.duration);
        e.set("size", (int64_t)f.size);
        files.push(std::move(e));
    }
    j.set("files", std::move(files));
    j.set("complete", complete);
    return lib::write_file_atomic(lib::join_path(dir, kManifest), j.dump(true));
}

bool read_manifest(const std::string& dir, AudiobookDownload& d) {
    std::string text;
    if (!lib::read_file(lib::join_path(dir, kManifest), text)) return false;
    bool ok = false;
    const lib::Json j = lib::Json::parse(text, &ok);
    if (!ok || !j["complete"].boolean() || j["item_id"].str().empty()) return false;
    d.dir = dir;
    d.item_id = j["item_id"].str();
    d.server = j["server"].str();
    d.local = j["local"].boolean() && is_local_book(d.item_id);
    abs::Book& b = d.book;
    b.id = d.item_id;
    b.title = j["title"].str();
    b.subtitle = j["subtitle"].str();
    b.author = j["author"].str();
    b.narrator = j["narrator"].str();
    b.series = j["series"].str();
    b.description = j["description"].str();
    b.year = j["year"].str();
    b.duration = j["duration"].num();
    for (const lib::Json& c : j["chapters"].items()) b.chapters.push_back({c["start"].num(), c["end"].num(), c["title"].str()});
    for (const lib::Json& f : j["files"].items()) {
        abs::AudioFile a;
        a.filename = f["name"].str();
        a.mime = f["mime"].str();
        a.start = f["start"].num();
        a.duration = f["duration"].num();
        a.size = (uint64_t)f["size"].integer();
        d.bytes += a.size;
        b.files.push_back(a);
    }
    b.chapter_count = (int)b.chapters.size();
    b.file_count = (int)b.files.size();
    const std::string named = j["cover_file"].str();
    const std::string cover = lib::join_path(dir, named.empty() ? "cover.jpg" : named);
    if (lib::file_exists(cover)) d.cover = cover;
    for (const std::string& slot : platform::usb_mount_points())
        if (lib::starts_with(dir, slot + "/")) d.on_usb = true;
    return !b.files.empty();
}

std::string folder_name(const abs::Book& b) {
    std::string name = b.author.empty() ? b.title : b.author + " - " + b.title;
    return lib::safe_file_name(name) + " [" + b.id.substr(0, 8) + "]";
}

}  // namespace

namespace {

// What the player can play, by extension (MIME types as Audiobookshelf names them).
const char* audio_mime(const std::string& name) {
    const std::string ext = lib::file_extension(name);
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "m4a" || ext == "m4b" || ext == "mp4") return "audio/mp4";
    if (ext == "aac") return "audio/aac";
    if (ext == "flac") return "audio/flac";
    if (ext == "opus" || ext == "ogg" || ext == "oga") return "audio/ogg";
    return nullptr;
}

bool hidden_name(const std::string& name) {
    return name.empty() || name[0] == '.' || name[0] == '$' || name == "System Volume Information";
}

// Looks through `dir` for books: a folder with a manifest is one, so is a
// folder of audio files (imported); other folders are looked into, `depth` deep.
void find_books(const std::string& dir, int depth, std::vector<AudiobookDownload>& out) {
    std::vector<lib::DirEntry> entries;
    if (!lib::list_dir(dir, entries, false)) return;
    for (const auto& e : entries) {
        if (!e.is_dir || hidden_name(e.name)) continue;
        const std::string sub = lib::join_path(dir, e.name);
        AudiobookDownload d;
        if (read_manifest(sub, d) || import_audio_folder(sub, d)) out.push_back(std::move(d));
        else if (depth > 1) find_books(sub, depth - 1, out);
    }
}

}  // namespace

// Not on the Audiobookshelf server: the listener's own folders, and LibriVox's books.
bool is_local_book(const std::string& id) { return lib::starts_with(id, "local-") || librivox::is_librivox(id); }

bool import_audio_folder(const std::string& dir, AudiobookDownload& out) {
    std::vector<lib::DirEntry> entries;
    if (!lib::list_dir(dir, entries, true)) return false;
    // A manifest that could not be read (a download still in progress, or
    // damaged): not the listener's own folder, so left alone.
    for (const auto& e : entries)
        if (e.name == kManifest) return false;
    // A DAISY talking book is read with its text and navigation (doc/daisy.h).
    for (const auto& e : entries)
        if (lib::iequals(e.name, "ncc.html") || lib::ends_with(lib::lower(e.name), ".opf")) return false;
    std::vector<lib::DirEntry> audio, pictures;
    for (const auto& e : entries) {
        if (e.is_dir || hidden_name(e.name) || lib::ends_with(e.name, ".part")) continue;
        if (audio_mime(e.name)) audio.push_back(e);
        const std::string ext = lib::file_extension(e.name);
        if (ext == "jpg" || ext == "jpeg" || ext == "png") pictures.push_back(e);
    }
    if (audio.empty()) return false;
    std::sort(audio.begin(), audio.end(), [](const auto& a, const auto& b) { return lib::natural_less(a.name, b.name); });

    abs::Book b;
    const std::string folder = lib::file_name(dir);
    // "Author - Title", the usual way such folders are named.
    const size_t dash = folder.find(" - ");
    if (dash != std::string::npos && dash > 0) {
        b.author = lib::trim(folder.substr(0, dash));
        b.title = lib::trim(folder.substr(dash + 3));
    } else {
        b.title = folder;
    }
    uint64_t total_bytes = 0;
    double start = 0;
    for (const auto& e : audio) {
        // Each file is opened once to learn its length (MP3s without an index
        // are estimated from their bit rate, as when they play).
        std::string error;
        const std::string path = lib::join_path(dir, e.name);
        auto source = audio::open_file_source(path, error);
        std::unique_ptr<audio::Decoder> decoder = source ? audio::make_decoder(source, audio_mime(e.name)) : nullptr;
        if (!decoder || !decoder->open(error) || decoder->duration() <= 0) {
            LOG("audiobooks: %s left out (%s)", path.c_str(), error.empty() ? "no length" : error.c_str());
            continue;
        }
        abs::AudioFile f;
        f.filename = e.name;
        f.mime = audio_mime(e.name);
        f.start = start;
        f.duration = decoder->duration();
        f.size = e.size;
        start += f.duration;
        total_bytes += e.size;
        b.files.push_back(f);
    }
    if (b.files.empty()) return false;
    b.duration = start;
    // Stable while the folder keeps its name and files, in any slot.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : lib::lower(folder) + "|" + std::to_string(total_bytes)) h = (h ^ c) * 1099511628211ull;
    char id[32];
    snprintf(id, sizeof id, "local-%016llx", (unsigned long long)h);
    b.id = id;
    // Each file a chapter, named after it.
    if (b.files.size() > 1)
        for (const auto& f : b.files) b.chapters.push_back({f.start, f.start + f.duration, lib::file_stem(f.filename)});
    std::string cover;
    for (const auto& p : pictures) {
        const std::string stem = lib::lower(lib::file_stem(p.name));
        if (cover.empty() || stem == "cover" || stem == "folder" || stem == "front") cover = p.name;
    }
    if (!write_manifest(dir, b, true, true, cover)) LOG("audiobooks: could not write the manifest in %s", dir.c_str());
    LOG("audiobooks: %s: %zu files, %s, made playable", dir.c_str(), b.files.size(), format_duration(b.duration).c_str());
    // As read back, or as made here when the folder cannot be written to.
    if (read_manifest(dir, out)) return true;
    out = AudiobookDownload();
    out.dir = dir;
    out.item_id = b.id;
    out.local = true;
    out.book = b;
    out.bytes = total_bytes;
    if (!cover.empty()) out.cover = lib::join_path(dir, cover);
    for (const std::string& slot : platform::usb_mount_points())
        if (lib::starts_with(dir, slot + "/")) out.on_usb = true;
    return true;
}

std::vector<AudiobookDownload> find_audiobook_downloads() {
    std::vector<AudiobookDownload> out;
    // The console's audiobooks folder, and Librarian's folder on each drive
    // (its Audiobooks folder holds the downloads; the listener's own folders
    // may be anywhere in it, a few levels deep).
    find_books(lib::join_path(platform::data_root(), "audiobooks"), 2, out);
    for (const std::string& slot : platform::usb_mount_points()) {
        std::vector<lib::DirEntry> e;
        if (lib::list_dir(slot, e, false) && !e.empty()) find_books(lib::join_path(slot, "Librarian"), 3, out);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return lib::natural_less(a.book.title, b.book.title); });
    return out;
}

bool audiobook_offline_playable(const abs::Book& book, std::string& why) {
    for (const auto& f : book.files)
        if (!audio::direct_play_mime(f.mime)) {
            why = "This audiobook is in a format (" + (f.codec.empty() ? f.mime : f.codec) +
                  ") that the server converts while it plays, so it can be streamed but not downloaded.";
            return false;
        }
    if (book.files.empty()) {
        why = "The server lists no audio files for this book.";
        return false;
    }
    return true;
}

bool download_audiobook(const abs::Book& book, const std::function<bool(uint64_t, uint64_t)>& progress,
                        AudiobookDownload& out, std::string& error) {
    if (!audiobook_offline_playable(book, error)) return false;
    const std::string usb = settings().download_to_usb ? first_connected_usb() : std::string();
    const std::string root = usb.empty() ? lib::join_path(platform::data_root(), "audiobooks")
                                         : lib::join_path(usb, "Librarian/Audiobooks");
    const std::string dir = lib::join_path(root, folder_name(book));
    if (!lib::make_dirs(dir)) {
        error = "Could not create " + dir + ".";
        return false;
    }
    // Local names: the server's, made safe and unique.
    abs::Book local = book;
    for (size_t i = 0; i < local.files.size(); ++i) {
        std::string name = lib::safe_file_name(local.files[i].filename.empty() ? "part" : local.files[i].filename);
        for (size_t k = 0; k < i; ++k)
            if (local.files[k].filename == name) name = std::to_string(i + 1) + " " + name;
        local.files[i].filename = name;
    }
    if (!write_manifest(dir, local, false)) {
        error = "Could not write to " + dir + " (is the storage full?).";
        return false;
    }
    uint64_t total = 0, done = 0;
    for (const auto& f : local.files) total += f.size;
    LOG("abs: downloading %s (%zu files, %s) to %s", book.title.c_str(), local.files.size(), lib::format_bytes(total).c_str(),
        dir.c_str());
    for (size_t i = 0; i < local.files.size(); ++i) {
        const auto& f = local.files[i];
        const std::string path = lib::join_path(dir, f.filename);
        // Already there from an interrupted download.
        if (f.size && lib::file_exists(path) && lib::file_size(path) == f.size) {
            done += f.size;
            continue;
        }
        if (!book.files[i].url.empty()) {
            // A file elsewhere (LibriVox): fetched directly, resumable.
            lib::HttpRequest request;
            request.url = book.files[i].url;
            request.timeout_seconds = 60;
            uint64_t got = 0;
            const lib::HttpResponse r = lib::http_download(request, path, [&](uint64_t d, uint64_t) {
                got = d;
                return !progress || progress(done + d, total ? total : 0);
            }, true);
            if (!r.ok()) {
                error = lib::describe_failure(r);
                return false;
            }
            done += got;
            continue;
        }
        const abs::Status st = abs::download_file(book.id, book.files[i].ino, path, [&](uint64_t d, uint64_t) {
            return !progress || progress(done + d, total);
        });
        if (!st.ok()) {
            error = st.error;
            return false;
        }
        done += f.size;
    }
    if (!book.cover_url.empty()) {
        lib::HttpRequest request;
        request.url = book.cover_url;
        lib::http_download(request, lib::join_path(dir, "cover.jpg"), nullptr);
    } else {
        abs::download_cover(book.id, lib::join_path(dir, "cover.jpg"));
    }
    // Sizes are known now (LibriVox lists none).
    for (auto& f : local.files)
        if (!f.size) f.size = lib::file_size(lib::join_path(dir, f.filename));
    if (!write_manifest(dir, local, true) || !read_manifest(dir, out)) {
        error = "Could not finish the download in " + dir + ".";
        return false;
    }
    return true;
}

bool delete_audiobook_download(const AudiobookDownload& d) {
    if (d.local) return false;  // the listener's own files are never deleted
    bool ok = true;
    for (const auto& f : d.book.files) {
        const std::string p = lib::join_path(d.dir, f.filename);
        if (lib::file_exists(p)) ok = lib::remove_file(p) && ok;
    }
    lib::remove_file(lib::join_path(d.dir, "cover.jpg"));
    ok = lib::remove_file(lib::join_path(d.dir, kManifest)) && ok;
    lib::remove_dir(d.dir);
    return ok;
}

std::vector<audio::Track> download_tracks(const AudiobookDownload& d) {
    std::vector<audio::Track> tracks;
    for (const auto& f : d.book.files) {
        audio::Track t;
        t.kind = audio::Track::File;
        t.location = lib::join_path(d.dir, f.filename);
        t.mime = f.mime;
        t.size = f.size;
        t.start = f.start;
        t.duration = f.duration;
        tracks.push_back(t);
    }
    return tracks;
}

// ---- the listening state -------------------------------------------------------------------

namespace {
std::string state_path() { return lib::join_path(platform::data_root(), "audiobook_state.json"); }
}  // namespace

Listening& listening() {
    static Listening instance;
    return instance;
}

void Listening::start() {
    std::string text;
    if (lib::read_file(state_path(), text)) {
        bool ok = false;
        const lib::Json root = lib::Json::parse(text, &ok);
        for (const auto& [id, j] : root["items"].members()) {
            ItemState& s = items_[id];
            s.progress.time = j["time"].num();
            s.progress.duration = j["duration"].num();
            s.progress.finished = j["finished"].boolean();
            s.progress.updated = j["updated"].integer();
            s.progress.synced = j["synced"].boolean(true);
            s.title = j["title"].str();
            s.author = j["author"].str();
            s.cover = j["cover"].str();
            s.hidden = j["hidden"].boolean();
            for (const lib::Json& b : j["bookmarks"].items())
                s.bookmarks.push_back({id, b["title"].str(), b["time"].num(), b["created"].integer()});
            for (const lib::Json& p : j["pending"].items()) s.pending.push_back({p["add"].boolean(), p["time"].num(), p["title"].str()});
        }
        if (!ok) LOG("audiobook_state.json is damaged; starting afresh");
    }
    refresh_downloads();
}

std::vector<RecentListen> Listening::continue_listening(size_t max) const {
    std::vector<RecentListen> out;
    for (const auto& [id, s] : items_) {
        if (s.title.empty() || s.hidden || s.progress.finished || s.progress.time < 5) continue;
        if (s.progress.duration > 0 && s.progress.time >= s.progress.duration - 10) continue;
        RecentListen r;
        r.id = id;
        r.title = s.title;
        r.author = s.author;
        r.time = s.progress.time;
        r.duration = s.progress.duration;
        r.updated = s.progress.updated;
        // A downloaded copy's own cover works with no server.
        if (const AudiobookDownload* d = download_of(id)) r.cover = d->cover;
        if (r.cover.empty()) r.cover = s.cover;
        if (r.cover.empty() && abs::configured() && !is_local_book(id)) r.cover = abs::cover_source(id);
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const RecentListen& a, const RecentListen& b) { return a.updated > b.updated; });
    if (out.size() > max) out.resize(max);
    return out;
}

void Listening::hide_from_continue(const std::string& id) {
    auto it = items_.find(id);
    if (it == items_.end()) return;
    it->second.hidden = true;
    dirty_ = true;
    save_local(true);
    changed();
}

void Listening::save_local(bool force) {
    if (!dirty_ && !force) return;
    lib::Json items = lib::Json::object();
    for (const auto& [id, s] : items_) {
        lib::Json j = lib::Json::object();
        j.set("time", s.progress.time);
        j.set("duration", s.progress.duration);
        j.set("finished", s.progress.finished);
        j.set("updated", (int64_t)s.progress.updated);
        j.set("synced", s.progress.synced);
        if (!s.title.empty()) j.set("title", s.title);
        if (!s.author.empty()) j.set("author", s.author);
        if (!s.cover.empty()) j.set("cover", s.cover);
        if (s.hidden) j.set("hidden", true);
        lib::Json marks = lib::Json::array();
        for (const auto& b : s.bookmarks) {
            lib::Json m = lib::Json::object();
            m.set("time", b.time);
            m.set("title", b.title);
            m.set("created", (int64_t)b.created);
            marks.push(std::move(m));
        }
        j.set("bookmarks", std::move(marks));
        lib::Json pending = lib::Json::array();
        for (const auto& p : s.pending) {
            lib::Json m = lib::Json::object();
            m.set("add", p.add);
            m.set("time", p.time);
            m.set("title", p.title);
            pending.push(std::move(m));
        }
        j.set("pending", std::move(pending));
        items.set(id, std::move(j));
    }
    lib::Json root = lib::Json::object();
    root.set("version", 1);
    root.set("items", std::move(items));
    if (!lib::write_file_atomic(state_path(), root.dump())) LOG("could not save audiobook_state.json");
    dirty_ = false;
    last_save_ = platform::now_ms();
}

std::string Listening::problem() const { return problem_; }

const AudiobookDownload* Listening::download_of(const std::string& id) const {
    for (const auto& d : downloads_)
        if (d.item_id == id) return &d;
    return nullptr;
}

void Listening::refresh_downloads() {
    lib::async<std::vector<AudiobookDownload>>(
        nullptr, [] { return find_audiobook_downloads(); },
        [this](std::vector<AudiobookDownload>& found) {
            downloads_ = std::move(found);
            changed();
            platform::request_redraw();
        },
        2);
}

LocalProgress Listening::progress(const std::string& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? LocalProgress() : it->second.progress;
}

double Listening::resume_position(const std::string& id, const abs::Progress& server) const {
    auto it = items_.find(id);
    const LocalProgress* local = it == items_.end() || !it->second.progress.updated ? nullptr : &it->second.progress;
    // The newer of the two: the console's, unless it was synced or the server's is newer.
    if (server.found && (!local || local->synced || server.updated >= local->updated))
        return server.finished ? 0 : server.current_time;
    if (local) return local->finished ? 0 : local->time;
    return 0;
}

void Listening::play(const abs::Book& book, double at) {
    if (active()) stop();
    problem_.clear();
    const int request = ++start_request_;
    if (const AudiobookDownload* d = download_of(book.id)) {
        // A downloaded copy plays at once, without the network.
        abs::Book b = d->book;
        const double pos = at >= 0 ? at : resume_position(book.id, book.progress);
        begin(b, download_tracks(*d), nullptr, pos, true);
        if (!d->cover.empty()) now_.cover = d->cover;
        if (abs::configured() && !is_local_book(book.id)) {
            // A session as well, when the server is there, so listening is
            // recorded; without one, progress is sent when it is back.
            const std::string id = book.id;
            lib::async<std::pair<abs::Status, abs::Session>>(
                nullptr,
                [id] {
                    std::pair<abs::Status, abs::Session> r;
                    r.first = abs::start_session(id, r.second);
                    return r;
                },
                [this, request, id](std::pair<abs::Status, abs::Session>& r) {
                    if (request != start_request_ || now_.id != id) {
                        if (r.first.ok()) abs::close_session(r.second.id, 0, 0, 0);
                        return;
                    }
                    if (r.first.ok()) now_.session = r.second.id;
                    else LOG("abs: playing offline (%s)", r.first.error.c_str());
                });
        }
        return;
    }
    if (librivox::is_librivox(book.id)) {
        // Streamed from archive.org; no server involved.
        if (book.files.empty()) {
            problem_ = "This book's chapters are not loaded yet.";
            return;
        }
        std::vector<audio::Track> tracks;
        for (const auto& f : book.files) {
            audio::Track t;
            t.kind = audio::Track::Http;
            t.location = f.url;
            t.mime = f.mime;
            t.size = f.size;
            t.start = f.start;
            t.duration = f.duration;
            tracks.push_back(t);
        }
        begin(book, std::move(tracks), nullptr, at >= 0 ? at : resume_position(book.id, book.progress), false);
        return;
    }
    if (!abs::configured()) {
        problem_ = "This book is not downloaded, and no Audiobookshelf server is set up.";
        return;
    }
    starting_ = true;
    now_ = NowPlaying();
    now_.id = book.id;
    now_.title = book.title;
    now_.author = book.author;
    now_.narrator = book.narrator;
    now_.cover = abs::cover_source(book.id);
    now_.duration = book.duration;
    changed();
    struct Result {
        abs::Status status;
        abs::Book book;
        abs::Session session;
    };
    const abs::Book given = book;
    lib::async<Result>(
        nullptr,
        [given] {
            Result r;
            r.book = given;
            // Chapters come with the full item.
            if (given.chapters.empty() && given.files.empty()) {
                r.status = abs::item(given.id, r.book);
                if (!r.status.ok()) return r;
            }
            r.status = abs::start_session(given.id, r.session);
            return r;
        },
        [this, request, at](Result& r) {
            if (request != start_request_) {
                if (r.status.ok()) abs::close_session(r.session.id, r.session.current_time, 0, r.session.duration);
                return;
            }
            starting_ = false;
            if (!r.status.ok()) {
                problem_ = r.status.error;
                now_ = NowPlaying();
                changed();
                return;
            }
            abs::Progress server = r.book.progress;
            // The session starts where the server last saw the listener.
            if (!server.found && r.session.current_time > 0) {
                server.found = true;
                server.current_time = r.session.current_time;
            }
            const double pos = at >= 0 ? at : resume_position(r.book.id, server);
            if (r.book.chapters.empty()) r.book.chapters = r.session.chapters;
            if (r.book.duration <= 0) r.book.duration = r.session.duration;
            begin(r.book, r.session.tracks, abs::auth_headers(), pos, false);
            now_.session = r.session.id;
        });
}

void Listening::begin(const abs::Book& book, std::vector<audio::Track> tracks, audio::HeaderFn headers, double at, bool local) {
    now_ = NowPlaying();
    now_.id = book.id;
    now_.title = book.title;
    now_.author = book.author;
    now_.narrator = book.narrator;
    // Audiobookshelf's own books have their covers there; the rest (LibriVox
    // books without one, the listener's folders) are looked up.
    now_.cover = !book.cover_url.empty() ? book.cover_url
                 : is_local_book(book.id) ? doc::find_cover_key(book.title, book.author)
                                          : abs::cover_source(book.id);
    now_.duration = book.duration > 0 ? book.duration : (tracks.empty() ? 0 : tracks.back().start + tracks.back().duration);
    now_.chapters = book.chapters;
    if (now_.chapters.empty() && tracks.size() > 1)
        for (size_t i = 0; i < tracks.size(); ++i)
            now_.chapters.push_back({tracks[i].start, tracks[i].start + tracks[i].duration, "Part " + std::to_string(i + 1)});
    now_.local = local;
    if (at >= now_.duration - 1) at = 0;  // finished books start over
    LOG("abs: playing %s from %s (%s)", book.title.c_str(), format_duration(at).c_str(), local ? "downloaded" : "streamed");
    audio::player().open(std::move(tracks), std::move(headers), at, true, settings().listen_speed / 100.0);
    listened_ = 0;
    listened_from_ = last_sync_ = last_save_ = platform::now_ms();
    last_position_ = at;
    ItemState& s = items_[book.id];
    s.progress.duration = now_.duration;
    s.title = book.title;
    s.author = book.author;
    if (!book.cover_url.empty()) s.cover = book.cover_url;
    s.hidden = false;
    dirty_ = true;
    starting_ = false;
    changed();
}

void Listening::stop() {
    if (!active()) return;
    tick();  // record where it stopped
    save_local(true);
    sync(true);
    audio::player().close();
    now_ = NowPlaying();
    sleep_until_ = 0;
    sleep_chapter_end_ = -1;
    sleep_minutes_ = 0;
    changed();
}

void Listening::tick() {
    const uint64_t now = platform::now_ms();
    if (!active()) {
        if (now - last_flush_ > 60000) sync_with_server();
        return;
    }
    audio::Player& p = audio::player();
    const audio::State st = p.state();
    if (st == audio::State::Playing) listened_ += (double)(now - listened_from_) / 1000.0;
    listened_from_ = now;
    if (st == audio::State::Error) {
        if (problem_.empty()) problem_ = p.error();
        return;
    }
    if (starting_ || st == audio::State::Loading) return;
    const double pos = p.position();
    ItemState& s = items_[now_.id];
    const bool moved = fabs(pos - last_position_) >= 1.0;
    const bool paused_now = st == audio::State::Paused && last_state_ != (int)audio::State::Paused;
    const bool ended_now = st == audio::State::Ended && last_state_ != (int)audio::State::Ended;
    last_state_ = (int)st;
    if (moved || paused_now || ended_now) {
        s.progress.time = ended_now ? now_.duration : pos;
        s.progress.duration = now_.duration;
        s.progress.finished = ended_now || (s.progress.finished && !moved);
        s.progress.updated = epoch_ms();
        s.progress.synced = false;
        last_position_ = pos;
        dirty_ = true;
        changed();
    }
    if (dirty_ && (now - last_save_ > 5000 || paused_now || ended_now)) save_local(true);
    if ((st == audio::State::Playing && now - last_sync_ > 15000) || paused_now || ended_now) sync(false);
    // The sleep timer.
    if (sleep_until_ && now >= sleep_until_) {
        p.pause();
        sleep_until_ = 0;
        sleep_minutes_ = 0;
        LOG("abs: sleep timer: paused");
    }
    if (sleep_chapter_end_ >= 0 && pos >= sleep_chapter_end_ - 0.05) {
        p.pause();
        sleep_chapter_end_ = -1;
        sleep_minutes_ = 0;
        LOG("abs: sleep timer: paused at the end of the chapter");
    }
    if (st == audio::State::Playing) problem_.clear();
}

void Listening::sync(bool closing) {
    if (!active() || !abs::configured() || is_local_book(now_.id)) return;
    if (syncing_ && !closing) return;
    const ItemState& s = items_[now_.id];
    const std::string id = now_.id, session = now_.session;
    const double pos = s.progress.time, duration = now_.duration, listened = listened_;
    const bool finished = s.progress.finished;
    const int64_t stamp = s.progress.updated;
    listened_ = 0;
    syncing_ = true;
    last_sync_ = platform::now_ms();
    lib::async<std::pair<abs::Status, bool>>(
        nullptr,
        [id, session, pos, duration, listened, finished, closing] {
            std::pair<abs::Status, bool> r{abs::Status(), false};
            if (!session.empty() && !finished) {
                r.first = closing ? abs::close_session(session, pos, listened, duration)
                                  : abs::sync_session(session, pos, listened, duration);
                if (r.first.ok()) return r;
                // The server forgot the session (it was idle, or restarted).
                if (r.first.status != 404) return r;
                r.second = true;
            }
            r.first = abs::set_progress(id, pos, duration, finished ? abs::Finished::Yes : abs::Finished::Leave);
            return r;
        },
        [this, id, stamp](std::pair<abs::Status, bool>& r) {
            syncing_ = false;
            if (r.second && now_.id == id) now_.session.clear();
            if (!r.first.ok()) {
                LOG("abs: progress not sent (%s); kept on the console", r.first.error.c_str());
                return;
            }
            auto it = items_.find(id);
            if (it != items_.end() && it->second.progress.updated == stamp) {
                it->second.progress.synced = true;
                dirty_ = true;
            }
        });
}

bool Listening::has_unsynced() const {
    for (const auto& [id, s] : items_)
        if (!is_local_book(id) && (!s.progress.synced || !s.pending.empty())) return true;
    return false;
}

void Listening::sync_with_server() {
    last_flush_ = platform::now_ms();
    if (!abs::configured() || flushing_) return;
    flushing_ = true;
    struct Work {
        std::string id;
        LocalProgress progress;
        std::vector<Pending> pending;
    };
    std::vector<Work> work;
    for (const auto& [id, s] : items_) {
        if (is_local_book(id)) continue;  // the listener's own folders stay on the console
        // The book playing reports its progress through its play session.
        const bool send_progress = !s.progress.synced && id != now_.id;
        if (!send_progress && s.pending.empty()) continue;
        Work w{id, s.progress, s.pending};
        if (!send_progress) w.progress.synced = true;
        work.push_back(w);
    }
    struct Result {
        abs::Status status;
        std::map<std::string, abs::Progress> progress;
        std::vector<abs::Bookmark> bookmarks;
        std::vector<std::string> synced;                         // progress sent
        std::vector<std::pair<std::string, size_t>> applied;    // bookmark changes sent, per book
    };
    lib::async<Result>(
        nullptr,
        [work] {
            Result r;
            r.status = abs::me(r.progress, r.bookmarks);
            if (!r.status.ok()) return r;
            bool changed_server = false;
            for (const Work& w : work) {
                if (!w.progress.synced) {
                    auto server = r.progress.find(w.id);
                    // The newer one wins: send ours only if the server's is older.
                    if (server == r.progress.end() || server->second.updated < w.progress.updated) {
                        if (abs::set_progress(w.id, w.progress.time, w.progress.duration,
                                              w.progress.finished ? abs::Finished::Yes : abs::Finished::Leave)
                                .ok()) {
                            r.synced.push_back(w.id);
                            changed_server = true;
                        }
                    } else {
                        r.synced.push_back(w.id);
                    }
                }
                size_t sent = 0;
                for (const Pending& p : w.pending) {
                    const abs::Status st = p.add ? abs::add_bookmark(w.id, p.time, p.title) : abs::remove_bookmark(w.id, p.time);
                    // 404 on a removal: it is already gone.
                    if (!st.ok() && !(st.status == 404 && !p.add)) break;
                    ++sent;
                    changed_server = true;
                }
                if (sent) r.applied.push_back({w.id, sent});
            }
            if (changed_server) abs::me(r.progress, r.bookmarks);
            return r;
        },
        [this](Result& r) {
            flushing_ = false;
            if (!r.status.ok()) return;
            for (const std::string& id : r.synced) items_[id].progress.synced = true;
            for (const auto& [id, n] : r.applied) {
                auto& p = items_[id].pending;
                p.erase(p.begin(), p.begin() + (long)std::min(n, p.size()));
            }
            merge_server(r.progress, r.bookmarks);
            dirty_ = true;
            save_local(true);
            changed();
        });
}

void Listening::merge_server(const std::map<std::string, abs::Progress>& progress, const std::vector<abs::Bookmark>& bookmarks) {
    for (const auto& [id, sp] : progress) {
        ItemState& s = items_[id];
        if (s.progress.synced || !s.progress.updated || sp.updated >= s.progress.updated) {
            s.progress.time = sp.current_time;
            s.progress.duration = sp.duration;
            s.progress.finished = sp.finished;
            s.progress.updated = sp.updated;
            s.progress.synced = true;
        }
    }
    std::map<std::string, std::vector<abs::Bookmark>> by_item;
    for (const auto& b : bookmarks) by_item[b.item_id].push_back(b);
    for (auto& [id, s] : items_) {
        auto found = by_item.find(id);
        s.bookmarks = found == by_item.end() ? std::vector<abs::Bookmark>() : found->second;
        // Changes not yet on the server stay visible.
        for (const Pending& p : s.pending) {
            if (p.add) s.bookmarks.push_back({id, p.title, p.time, epoch_ms()});
            else s.bookmarks.erase(std::remove_if(s.bookmarks.begin(), s.bookmarks.end(),
                                                  [&](const abs::Bookmark& b) { return (int64_t)b.time == (int64_t)p.time; }),
                                   s.bookmarks.end());
        }
        std::sort(s.bookmarks.begin(), s.bookmarks.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    }
    for (auto& [id, marks] : by_item)
        if (!items_.count(id)) {
            items_[id].bookmarks = marks;
        }
    dirty_ = true;
    changed();
}

std::vector<abs::Bookmark> Listening::bookmarks(const std::string& id) const {
    auto it = items_.find(id);
    return it == items_.end() ? std::vector<abs::Bookmark>() : it->second.bookmarks;
}

void Listening::add_bookmark(const std::string& id, double time, const std::string& title) {
    time = floor(time);  // the server keeps them by whole second
    ItemState& s = items_[id];
    for (const auto& b : s.bookmarks)
        if ((int64_t)b.time == (int64_t)time) return;
    s.bookmarks.push_back({id, title, time, epoch_ms()});
    std::sort(s.bookmarks.begin(), s.bookmarks.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    s.pending.push_back({true, time, title});
    dirty_ = true;
    save_local(true);
    changed();
    sync_with_server();
}

void Listening::remove_bookmark(const std::string& id, double time) {
    ItemState& s = items_[id];
    s.bookmarks.erase(std::remove_if(s.bookmarks.begin(), s.bookmarks.end(),
                                     [&](const abs::Bookmark& b) { return (int64_t)b.time == (int64_t)time; }),
                      s.bookmarks.end());
    // An addition not yet sent is simply dropped.
    auto added = std::find_if(s.pending.begin(), s.pending.end(), [&](const Pending& p) { return p.add && (int64_t)p.time == (int64_t)time; });
    if (added != s.pending.end()) s.pending.erase(added);
    else s.pending.push_back({false, time, ""});
    dirty_ = true;
    save_local(true);
    changed();
    sync_with_server();
}

void Listening::mark_finished(const std::string& id, double duration, bool finished) {
    ItemState& s = items_[id];
    s.progress.finished = finished;
    s.progress.time = finished ? duration : 0;
    s.progress.duration = duration;
    s.progress.updated = epoch_ms();
    s.progress.synced = false;
    dirty_ = true;
    save_local(true);
    changed();
    if (!abs::configured() || is_local_book(id)) return;
    const int64_t stamp = s.progress.updated;
    lib::async<abs::Status>(
        nullptr,
        [id, duration, finished] {
            // An explicit choice of the listener's, so it is sent either way.
            return abs::set_progress(id, finished ? duration : 0, duration, finished ? abs::Finished::Yes : abs::Finished::No);
        },
        [this, id, stamp](abs::Status& st) {
            if (st.ok() && items_[id].progress.updated == stamp) {
                items_[id].progress.synced = true;
                dirty_ = true;
            }
        });
}

// ---- controls ------------------------------------------------------------------------------

void Listening::toggle() {
    audio::Player& p = audio::player();
    if (p.state() == audio::State::Ended) {
        p.seek(0);
        p.play();
        return;
    }
    if (p.state() == audio::State::Error && active()) {
        // Try again from where it was.
        problem_.clear();
        p.seek(p.position());
        p.play();
        return;
    }
    p.toggle();
}

void Listening::seek(double t) {
    if (!active()) return;
    audio::player().seek(std::max(0.0, std::min(t, now_.duration)));
}

void Listening::skip(double seconds) { seek(audio::player().position() + seconds); }

int Listening::chapter_at(double t) const {
    int found = -1;
    for (int i = 0; i < (int)now_.chapters.size(); ++i)
        if (now_.chapters[i].start <= t + 0.01) found = i;
    return found;
}

void Listening::next_chapter() {
    const int i = chapter_at(audio::player().position());
    if (i + 1 < (int)now_.chapters.size()) seek(now_.chapters[i + 1].start);
}

void Listening::previous_chapter() {
    const double pos = audio::player().position();
    const int i = chapter_at(pos);
    if (i < 0) {
        seek(0);
        return;
    }
    if (pos - now_.chapters[i].start > 3 || i == 0) seek(now_.chapters[i].start);
    else seek(now_.chapters[i - 1].start);
}

void Listening::set_speed(double speed) {
    speed = std::max(0.5, std::min(3.0, round(speed * 20) / 20));
    audio::player().set_speed(speed);
    settings().listen_speed = (int)lround(speed * 100);
    settings_save();
}

void Listening::set_sleep(int minutes) {
    sleep_minutes_ = minutes;
    sleep_until_ = 0;
    sleep_chapter_end_ = -1;
    if (minutes > 0) sleep_until_ = platform::now_ms() + (uint64_t)minutes * 60000;
    if (minutes < 0) {
        const int i = chapter_at(audio::player().position());
        if (i >= 0) sleep_chapter_end_ = now_.chapters[i].end;
        else sleep_minutes_ = 0;
    }
}

std::string Listening::sleep_label() const {
    if (sleep_chapter_end_ >= 0) return "End of chapter";
    if (!sleep_until_) return "Off";
    const uint64_t now = platform::now_ms();
    return sleep_until_ > now ? format_duration((double)(sleep_until_ - now) / 1000) + " left" : "Off";
}

}  // namespace svc
