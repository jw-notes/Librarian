// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Collections (ZIM files) from the Kiwix online library: downloaded by the
// console to a USB drive or its own storage (one at a time, on a thread of
// their own: they can take hours), or handed to the download helper beside the
// user's Kiwix Server (extras/kiwix-helper), which fetches them itself.
#include <stdint.h>

#include <string>
#include <vector>

#include "services/kiwix.h"

namespace svc {

struct ZimJob {
    enum class State { Queued, Running, Done, Failed, Cancelled };
    int id = 0;
    std::string title, url, path;  // path: where the finished file goes
    uint64_t done = 0, total = 0;  // bytes
    State state = State::Queued;
    std::string error;
    bool on_usb = false;
};

// Where downloaded collections are kept: "<drive>/Librarian/Kiwix" on the
// first connected USB drive ("" when none), or "<data>/zim" on the console.
std::string zim_folder(bool usb);
// What is already there for `book`: the finished file, or the bytes of a
// download that stopped part-way (continued when it is started again).
struct ZimOnDisk {
    std::string path;
    bool complete = false;
    uint64_t partial = 0;
};
ZimOnDisk zim_on_disk(const ZimBook& book, bool usb);
// The largest file a FAT32 drive can hold (many USB sticks are FAT32; exFAT
// has no such limit).
constexpr uint64_t kFat32Limit = 4294967295ull;

class ZimDownloads {
public:
    // Queues the download; false (with a reason) when it cannot start.
    bool start(const ZimBook& book, bool to_usb, std::string& error);
    void cancel(int id);
    std::vector<ZimJob> jobs() const;
    // A job for this file, queued or running ("" url: none).
    bool active(const std::string& path) const;
    // Counts each change, so screens know when to redraw.
    uint64_t generation() const;
    // Stops the current download (its .part file stays, to be continued).
    void stop();
};
ZimDownloads& zim_downloads();

// ---- the download helper beside the Kiwix Server ------------------------------------

namespace kiwix_helper {

bool configured();
struct Job {
    std::string id, title, file, state, error;  // state: queued, running, done, failed, cancelled
    uint64_t done = 0, total = 0;
};
struct Status {
    std::string folder;
    uint64_t free_bytes = 0;
    bool updates_library = false;  // adds finished files to kiwix-serve's library itself
};
// All of these talk to the helper: call them on a worker thread.
bool status(Status& out, std::string& error);
bool send(const ZimBook& book, std::string& error);
bool jobs(std::vector<Job>& out, std::string& error);
bool cancel(const std::string& id, std::string& error);
// Parses the helper's job list (exposed for tests).
bool parse_jobs(const std::string& json, std::vector<Job>& out);

}  // namespace kiwix_helper
}  // namespace svc
