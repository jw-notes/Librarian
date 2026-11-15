// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The audiobook player: one book (one or more files, or a server stream)
// played on the console's audio output, independently of the screens, so it
// carries on while the rest of Librarian is used.
//
// Two threads: the engine decodes, converts to 48 kHz stereo and time-stretches
// into a ring of about 8 seconds; the output thread hands that to the device in
// real time. Memory stays bounded however long the book is.
#include <stdint.h>

#include <string>
#include <vector>

#include "audio/byte_source.h"

namespace audio {

struct Track {
    enum Kind { File, Http, Hls };
    Kind kind = File;
    std::string location;           // a path, a file's URL, or an HLS playlist URL
    std::string mime;
    uint64_t size = 0;              // Http: the file's size
    double start = 0, duration = 0; // its place in the book, in seconds
    // File: a part of a larger file (an audio file stored inside an EPUB),
    // `slice_size` bytes from `offset`. 0 = the whole file.
    uint64_t offset = 0, slice_size = 0;
    // Read-along: only this stretch of the file is played (seconds in the
    // file); the track's duration is clip_end - clip_begin. 0/0 = all of it.
    double clip_begin = 0, clip_end = 0;
};

enum class State { Idle, Loading, Buffering, Playing, Paused, Ended, Error };
const char* state_name(State s);

class Player {
public:
    Player();
    ~Player();
    // Replaces whatever is loaded and starts at `at` seconds into the book.
    void open(std::vector<Track> tracks, HeaderFn headers, double at, bool playing, double speed);
    void play();
    void pause();
    void toggle() { state() == State::Paused ? play() : pause(); }
    void seek(double at);
    // 0.5 .. 3.0; the pitch is kept.
    void set_speed(double speed);
    void close();

    bool loaded() const;
    State state() const;
    // Where in the book the sound being played is.
    double position() const;
    double duration() const;
    double speed() const;
    std::string error() const;
    // Seconds of decoded sound waiting to be played.
    double buffered() const;
    // Stops the threads (at exit; tests).
    void shutdown();

private:
    struct Impl;
    Impl* impl_;
};

Player& player();

}  // namespace audio
