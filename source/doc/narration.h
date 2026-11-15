// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Read-along narration: which passage of the text is read when, from SMIL
// files (EPUB 3 media overlays, DAISY 2.02 and DAISY 3 talking books), and the
// timeline the audio player plays it on.
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "audio/player.h"

namespace doc {

// One narrated passage.
struct SyncPoint {
    int section = -1;           // the text section (chapter) it is in; -1: audio only
    std::string id;             // the element's id in that section ("" with no text)
    std::string audio;          // the audio file (a path inside the book)
    double begin = 0, end = 0;  // seconds in that file
    double at = 0;              // seconds on the timeline (set by build_timeline)
    // Where it came from: the text target ("path#id"), the SMIL file and the
    // ids of its <par> and <text> there (navigation points at those).
    std::string text, smil, par_id, text_id;
};

// A SMIL clock value: "0:01:02.5", "01:02.5", "12.5s", "1500ms", "2min",
// "1.5h", "npt=12.5s", "12.5". -1 if it is not one.
double parse_clock(const std::string& value);

// Appends the passages of a SMIL file, in order. `smil_path`: where the file
// is (text and audio sources are relative to it); `section_of(path)`: the
// section showing that id of a text file (-1 if none, or not known yet).
bool parse_smil(const std::string& xml, const std::string& smil_path,
                const std::function<int(const std::string& path, const std::string& id)>& section_of,
                std::vector<SyncPoint>& out);

// The ids narrated in `section` (they become marks when it is converted).
std::set<std::string> narrated_ids(const std::vector<SyncPoint>& points, int section);

struct Timeline {
    std::vector<SyncPoint> points;    // in reading order; `at` ascending
    std::vector<audio::Track> tracks;
    double duration = 0;
    // The passage being read at `t` seconds (-1 before the first).
    int find(double t) const;
    // The first passage at or after (section, block), using `block_of(id)`
    // for that section's passages; -1 if none.
    int first_from(int section, int block, const std::function<int(const std::string& id)>& block_of) const;
};

// Puts `points` on one timeline: clips that follow on in the same file become
// one track. `locate(audio, track)` fills a track's location (and offset /
// slice for a file inside an archive) and MIME type; passages whose audio
// cannot be located are left out.
void build_timeline(std::vector<SyncPoint> points, const std::function<bool(const std::string&, audio::Track&)>& locate,
                    Timeline& out);

// The folder for audio copied out of `book` (compressed audio inside an EPUB
// or a zipped DAISY book): copies made for any other book are deleted, so
// only the book being read keeps any.
std::string narration_cache(const std::string& root, const std::string& book);

// The MIME type for an audio file name ("" if not audio Librarian plays).
std::string audio_mime_of(const std::string& name);

}  // namespace doc
