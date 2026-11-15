// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Listening to audiobooks: what is playing, its play session on the server,
// progress kept on the console and synced with Audiobookshelf, bookmarks, the
// sleep timer, and downloaded copies for offline listening.
//
// Progress is saved locally every few seconds and on pause. With the server
// reachable it is synced through the play session; without it, it is marked
// unsynced and sent when the server is back (the newer of the two wins).
// Bookmark changes made offline wait in a queue the same way.
#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "services/audiobookshelf.h"

namespace svc {

// A book downloaded for offline listening: a folder with its audio files, its
// cover and librarian-audiobook.json (metadata, chapters, the files' places).
struct AudiobookDownload {
    std::string dir, item_id, server;
    abs::Book book;  // files[].filename is the local file name
    std::string cover;  // path, or empty
    bool on_usb = false;
    // The listener's own folder of audio files (not from a server): its id
    // starts with "local-", it is never synced, and its files are never deleted.
    bool local = false;
    uint64_t bytes = 0;
};

// True for a book made from the listener's own folder (see AudiobookDownload::local).
bool is_local_book(const std::string& id);

// Downloaded books on the console and connected USB drives (lists folders),
// and folders of the listener's own audio files inside Librarian's folders:
// <drive>/Librarian/ on USB drives and the console's audiobooks folder. Such a
// folder is measured once and gets a librarian-audiobook.json (marked local),
// so it then plays like a download.
std::vector<AudiobookDownload> find_audiobook_downloads();
// Makes `dir` (audio files, no manifest) playable as described above. False if
// it holds no audio file Librarian can play.
bool import_audio_folder(const std::string& dir, AudiobookDownload& out);
// Can this book be played from a download? (Formats the server would have to
// transcode, such as Opus, cannot.)
bool audiobook_offline_playable(const abs::Book& book, std::string& why);
// Downloads every file of `book` (from item(), with its files), resuming files
// already complete. Blocking. Where: Settings › System › Download books to.
bool download_audiobook(const abs::Book& book, const std::function<bool(uint64_t done, uint64_t total)>& progress,
                        AudiobookDownload& out, std::string& error);
bool delete_audiobook_download(const AudiobookDownload& d);
// The book as seen when its folder was downloaded.
std::vector<audio::Track> download_tracks(const AudiobookDownload& d);

struct LocalProgress {
    double time = 0, duration = 0;
    bool finished = false;
    int64_t updated = 0;  // ms since 1970
    bool synced = true;
};

// A book listened to and not finished, for Home's Continue Listening row.
struct RecentListen {
    std::string id, title, author, cover;
    double time = 0, duration = 0;
    int64_t updated = 0;
};

struct NowPlaying {
    std::string id, title, author, narrator, cover;
    double duration = 0;
    std::vector<abs::Chapter> chapters;
    bool local = false;    // from a downloaded copy
    std::string session;   // the server's play session, if there is one
};

class Listening {
public:
    // Loads the saved progress and bookmarks (at startup).
    void start();
    // Every frame, on the UI thread: saving and syncing, the sleep timer, the end.
    void tick();

    // Plays a book: a downloaded copy when there is one, else streamed. `at`
    // < 0 resumes (the newer of the server's and the console's position).
    void play(const abs::Book& book, double at = -1);
    void stop();
    bool active() const { return !now_.id.empty(); }
    const NowPlaying& now() const { return now_; }
    bool starting() const { return starting_; }
    // A problem to show (cannot start, network lost, ...); empty if none.
    std::string problem() const;

    void toggle();
    void seek(double t);
    void skip(double seconds);
    void next_chapter();
    void previous_chapter();  // to the start of this chapter, or the one before within 3 s
    int chapter_at(double t) const;
    void set_speed(double speed);  // also remembered in Settings

    // 0 off, minutes, or -1 for the end of the current chapter.
    void set_sleep(int minutes);
    std::string sleep_label() const;

    // Progress as known on the console (with what the server said, merged).
    LocalProgress progress(const std::string& id) const;
    // Books started and not finished, most recently listened first.
    std::vector<RecentListen> continue_listening(size_t max) const;
    // Leaves a book out of continue_listening() until it is played again.
    void hide_from_continue(const std::string& id);
    double resume_position(const std::string& id, const abs::Progress& server) const;
    void mark_finished(const std::string& id, double duration, bool finished);

    // Bookmarks of a book: the server's, with changes not yet sent.
    std::vector<abs::Bookmark> bookmarks(const std::string& id) const;
    void add_bookmark(const std::string& id, double time, const std::string& title);
    void remove_bookmark(const std::string& id, double time);
    // Replaces the cached server state (from abs::me), keeping local changes.
    void merge_server(const std::map<std::string, abs::Progress>& progress, const std::vector<abs::Bookmark>& bookmarks);
    bool has_unsynced() const;
    // Sends what waits (offline progress, bookmark changes) and fetches the
    // server's progress and bookmarks. Runs in the background; also every minute.
    void sync_with_server();
    bool syncing_with_server() const { return flushing_; }

    // Downloads, cached: refresh() lists the folders again (on a worker).
    const std::vector<AudiobookDownload>& downloads() const { return downloads_; }
    const AudiobookDownload* download_of(const std::string& id) const;
    void refresh_downloads();
    uint64_t generation() const { return generation_; }

private:
    void save_local(bool force);
    void sync(bool closing);
    void begin(const abs::Book& book, std::vector<audio::Track> tracks, audio::HeaderFn headers, double at, bool local);
    void changed() { ++generation_; }

    NowPlaying now_;
    bool starting_ = false, syncing_ = false, flushing_ = false;
    std::string problem_;
    uint64_t last_save_ = 0, last_sync_ = 0, last_flush_ = 0, listened_from_ = 0, generation_ = 0;
    double listened_ = 0, last_position_ = 0;
    int sleep_minutes_ = 0, last_state_ = -1;
    uint64_t sleep_until_ = 0;
    double sleep_chapter_end_ = -1;
    int start_request_ = 0;

    struct Pending {
        bool add = true;
        double time = 0;
        std::string title;
    };
    struct ItemState {
        LocalProgress progress;
        std::string title, author, cover;  // for Continue Listening (cover: books not on the server)
        bool hidden = false;        // left out of Continue Listening
        std::vector<abs::Bookmark> bookmarks;  // as last seen on the server, plus local additions
        std::vector<Pending> pending;
    };
    std::map<std::string, ItemState> items_;
    bool dirty_ = false;
    std::vector<AudiobookDownload> downloads_;
};

Listening& listening();

// Epoch milliseconds (the console's clock).
int64_t epoch_ms();
std::string format_duration(double seconds);  // "1:02:03" or "12:34"

}  // namespace svc
