// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/narration.h"

#include <ctype.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>

#include "core/fs.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "doc/archive.h"

namespace doc {

double parse_clock(const std::string& raw) {
    std::string v = lib::trim(raw);
    if (lib::starts_with(v, "npt=")) v = v.substr(4);
    if (v.empty()) return -1;
    if (v.find(':') != std::string::npos) {
        // [hh:]mm:ss[.fraction]
        double total = 0;
        for (const std::string& part : lib::split(v, ':')) {
            char* end = nullptr;
            const double x = strtod(part.c_str(), &end);
            if (part.empty() || (end && *end)) return -1;
            total = total * 60 + x;
        }
        return total;
    }
    char* end = nullptr;
    const double x = strtod(v.c_str(), &end);
    if (end == v.c_str()) return -1;
    const std::string unit = lib::lower(lib::trim(end));
    if (unit.empty() || unit == "s") return x;
    if (unit == "ms") return x / 1000;
    if (unit == "min") return x * 60;
    if (unit == "h") return x * 3600;
    return -1;
}

std::string narration_cache(const std::string& root, const std::string& book) {
    const std::string owner = lib::join_path(root, "book.txt");
    std::string current;
    lib::read_file(owner, current, 4096);
    if (current != book) {
        std::vector<lib::DirEntry> entries;
        if (lib::list_dir(root, entries, false))
            for (const auto& e : entries)
                if (!e.is_dir) lib::remove_file(lib::join_path(root, e.name));
        lib::make_dirs(root);
        lib::write_file_atomic(owner, book);
    }
    return root;
}

std::string audio_mime_of(const std::string& name) {
    const std::string ext = lib::lower(name.substr(name.rfind('.') + 1));
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "m4a" || ext == "mp4" || ext == "m4b") return "audio/mp4";
    if (ext == "aac") return "audio/aac";
    if (ext == "flac") return "audio/flac";
    if (ext == "ogg" || ext == "oga" || ext == "opus") return "audio/ogg";
    return "";
}

namespace {

// clipBegin (EPUB 3, DAISY 3) or clip-begin (DAISY 2.02); the parser may
// have lower-cased attribute names.
std::string attr_any(const lib::Node& n, const char* a, const char* b) {
    for (const char* name : {a, b}) {
        if (!n.attr(name).empty()) return n.attr(name);
        const std::string lower = lib::lower(name);
        if (!n.attr(lower).empty()) return n.attr(lower);
    }
    return std::string();
}

}  // namespace

bool parse_smil(const std::string& xml, const std::string& smil_path,
                const std::function<int(const std::string& path, const std::string& id)>& section_of,
                std::vector<SyncPoint>& out) {
    auto root = lib::parse_markup(lib::to_utf8(xml), lib::MarkupMode::Xml);
    if (!root || !root->find("body")) return false;
    const size_t before = out.size();
    int last_section = -1;
    // Walks in document order; each <par> is a passage (a <text> with one or
    // more <audio> clips); an <audio> outside any <par> is narration alone.
    std::string par_id;
    std::function<void(const lib::Node&, const lib::Node*)> walk = [&](const lib::Node& n, const lib::Node* text) {
        for (const auto& child : n.children) {
            if (child->is_text()) continue;
            const std::string_view tag = child->local();
            if (tag == "par") {
                const lib::Node* t = nullptr;
                for (const auto& c : child->children)
                    if (!c->is_text() && c->local() == "text") t = c.get();
                const std::string saved = par_id;
                par_id = child->attr("id");
                walk(*child, t);
                par_id = saved;
            } else if (tag == "audio") {
                SyncPoint p;
                const std::string src = child->attr("src");
                if (src.empty()) continue;
                p.audio = archive_resolve(smil_path, src);
                p.begin = parse_clock(attr_any(*child, "clipBegin", "clip-begin"));
                p.end = parse_clock(attr_any(*child, "clipEnd", "clip-end"));
                if (p.begin < 0) p.begin = 0;
                if (p.end <= p.begin) continue;  // a clip "to the end of the file" is not timed; skipped
                p.smil = smil_path;
                p.par_id = par_id;
                if (text) {
                    const std::string target = archive_resolve(smil_path, text->attr("src"));
                    const size_t hash = target.find('#');
                    if (hash != std::string::npos) p.id = target.substr(hash + 1);
                    p.text = target;
                    p.text_id = text->attr("id");
                    p.section = section_of(hash == std::string::npos ? target : target.substr(0, hash), p.id);
                    if (p.section >= 0) last_section = p.section;
                } else {
                    p.section = last_section;
                }
                out.push_back(std::move(p));
            } else {
                walk(*child, text);
            }
        }
    };
    walk(*root->find("body"), nullptr);
    return out.size() > before;
}

std::set<std::string> narrated_ids(const std::vector<SyncPoint>& points, int section) {
    std::set<std::string> ids;
    for (const SyncPoint& p : points)
        if (p.section == section && !p.id.empty()) ids.insert(p.id);
    return ids;
}

int Timeline::find(double t) const {
    if (points.empty() || t < points.front().at) return -1;
    auto it = std::upper_bound(points.begin(), points.end(), t, [](double v, const SyncPoint& p) { return v < p.at; });
    return (int)(it - points.begin()) - 1;
}

int Timeline::first_from(int section, int block, const std::function<int(const std::string& id)>& block_of) const {
    int fallback = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        const SyncPoint& p = points[i];
        if (p.section < section) continue;
        if (p.section > section) return fallback >= 0 ? fallback : (int)i;
        if (fallback < 0) fallback = (int)i;
        const int b = p.id.empty() ? -1 : block_of(p.id);
        if (b >= block) return (int)i;
    }
    return fallback;
}

void build_timeline(std::vector<SyncPoint> points, const std::function<bool(const std::string&, audio::Track&)>& locate,
                    Timeline& out) {
    out = Timeline();
    std::string failed;
    double at = 0;
    for (SyncPoint& p : points) {
        audio::Track* t = out.tracks.empty() ? nullptr : &out.tracks.back();
        const bool follows = t && out.points.back().audio == p.audio && fabs(p.begin - t->clip_end) < 0.05;
        if (!follows) {
            audio::Track track;
            if (p.audio == failed || !locate(p.audio, track)) {
                if (p.audio != failed) LOG("narration: %s cannot be played", p.audio.c_str());
                failed = p.audio;
                continue;
            }
            track.kind = audio::Track::File;
            track.clip_begin = p.begin;
            track.clip_end = p.end;
            track.start = at;
            track.duration = p.end - p.begin;
            out.tracks.push_back(track);
            t = &out.tracks.back();
        } else {
            // One continuous stretch of the file: just longer.
            t->clip_end = std::max(t->clip_end, p.end);
            t->duration = t->clip_end - t->clip_begin;
        }
        p.at = t->start + (p.begin - t->clip_begin);
        at = t->start + t->duration;
        out.points.push_back(std::move(p));
    }
    out.duration = at;
}

}  // namespace doc
