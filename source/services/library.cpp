// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/library.h"

#include <time.h>

#include <algorithm>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/text.h"
#include "platform/platform.h"

namespace svc {

namespace {

constexpr size_t kMaxArticles = 300;

std::string path() { return lib::join_path(platform::data_root(), "library.json"); }

lib::Json position_json(const Position& p) {
    lib::Json j = lib::Json::object();
    j.set("section", p.section);
    j.set("block", p.block);
    j.set("offset", (double)p.offset);
    return j;
}

Position position_from(const lib::Json& j) {
    Position p;
    p.section = (int)j["section"].integer(0);
    p.block = (int)j["block"].integer(0);
    p.offset = (float)j["offset"].num(0);
    return p;
}

}  // namespace

Library& library() {
    static Library instance;
    return instance;
}

std::string article_id(const std::string& collection, const std::string& path) { return "kiwix:" + collection + "/" + path; }
std::string file_id(const std::string& path) { return "file:" + path; }

std::string file_fingerprint(const std::string& path, uint64_t size) {
    return lib::lower(lib::file_name(path)) + "|" + std::to_string(size);
}

void Library::load() {
    std::string text;
    if (!lib::read_file(path(), text)) return;
    bool ok = false;
    const lib::Json root = lib::Json::parse(text, &ok);
    if (!ok) {
        // Keep the damaged file for inspection rather than overwriting it.
        LOG("library.json is damaged; keeping a copy");
        rename(path().c_str(), (path() + ".damaged").c_str());
        return;
    }
    for (const lib::Json& j : root["items"].items()) {
        Item it;
        it.id = j["id"].str();
        if (it.id.empty()) continue;
        it.kind = j["kind"].str() == "article" ? ItemKind::Article : ItemKind::Book;
        it.source = j["source"].str();
        it.title = j["title"].str();
        it.author = j["author"].str();
        it.path = j["path"].str();
        it.url = j["url"].str();
        it.collection = j["collection"].str();
        it.collection_title = j["collection_title"].str();
        it.cover = j["cover"].str();
        it.format = j["format"].str();
        it.remote_id = j["remote_id"].str();
        it.fingerprint = j["fingerprint"].str();
        it.added = j["added"].integer();
        it.opened = j["opened"].integer();
        it.position = position_from(j["position"]);
        it.progress = (float)j["progress"].num(0);
        it.finished = j["finished"].boolean(false);
        it.favourite = j["favourite"].boolean(false);
        for (const lib::Json& b : j["bookmarks"].items())
            it.bookmarks.push_back({position_from(b["at"]), b["label"].str(), b["created"].integer()});
        items_.push_back(std::move(it));
    }
    LOG("library: %zu items", items_.size());
}

void Library::save_if_dirty() {
    if (!dirty_) return;
    lib::Json list = lib::Json::array();
    for (const Item& it : items_) {
        lib::Json j = lib::Json::object();
        j.set("id", it.id);
        j.set("kind", it.kind == ItemKind::Article ? "article" : "book");
        j.set("source", it.source);
        j.set("title", it.title);
        if (!it.author.empty()) j.set("author", it.author);
        if (!it.path.empty()) j.set("path", it.path);
        if (!it.url.empty()) j.set("url", it.url);
        if (!it.collection.empty()) j.set("collection", it.collection);
        if (!it.collection_title.empty()) j.set("collection_title", it.collection_title);
        if (!it.cover.empty()) j.set("cover", it.cover);
        if (!it.format.empty()) j.set("format", it.format);
        if (!it.remote_id.empty()) j.set("remote_id", it.remote_id);
        if (!it.fingerprint.empty()) j.set("fingerprint", it.fingerprint);
        j.set("added", (int64_t)it.added);
        j.set("opened", (int64_t)it.opened);
        j.set("position", position_json(it.position));
        j.set("progress", (double)it.progress);
        if (it.finished) j.set("finished", true);
        if (it.favourite) j.set("favourite", true);
        if (!it.bookmarks.empty()) {
            lib::Json marks = lib::Json::array();
            for (const Bookmark& b : it.bookmarks) {
                lib::Json m = lib::Json::object();
                m.set("at", position_json(b.at));
                m.set("label", b.label);
                m.set("created", (int64_t)b.created);
                marks.push(std::move(m));
            }
            j.set("bookmarks", std::move(marks));
        }
        list.push(std::move(j));
    }
    lib::Json root = lib::Json::object();
    root.set("version", 1);
    root.set("items", std::move(list));
    if (lib::write_file_atomic(path(), root.dump(true))) dirty_ = false;
    else LOG("could not save library.json");
}

Item* Library::find_file(const std::string& path, const std::string& fingerprint) {
    if (Item* it = find(file_id(path))) return it;
    if (fingerprint.empty()) return nullptr;
    for (Item& it : items_)
        if (it.kind == ItemKind::Book && it.fingerprint == fingerprint) return &it;
    return nullptr;
}

Item* Library::find(const std::string& id) {
    for (Item& it : items_)
        if (it.id == id) return &it;
    return nullptr;
}

Item& Library::upsert(const Item& item) {
    dirty_ = true;
    if (Item* existing = find(item.id)) {
        // Refresh descriptive fields; keep the reading state.
        if (!item.title.empty()) existing->title = item.title;
        if (!item.author.empty()) existing->author = item.author;
        if (!item.path.empty()) existing->path = item.path;
        if (!item.url.empty()) existing->url = item.url;
        if (!item.cover.empty()) existing->cover = item.cover;
        if (!item.format.empty()) existing->format = item.format;
        if (!item.collection_title.empty()) existing->collection_title = item.collection_title;
        if (!item.remote_id.empty()) existing->remote_id = item.remote_id;
        if (!item.fingerprint.empty()) existing->fingerprint = item.fingerprint;
        return *existing;
    }
    items_.push_back(item);
    Item& added = items_.back();
    if (!added.added) added.added = time(nullptr);
    // Article history is bounded: drop the oldest unbookmarked, unfavourited ones.
    size_t articles = 0;
    for (const Item& it : items_) articles += it.kind == ItemKind::Article;
    if (articles > kMaxArticles) {
        auto oldest = items_.end();
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->kind != ItemKind::Article || it->favourite || !it->bookmarks.empty() || it->id == item.id) continue;
            if (oldest == items_.end() || it->opened < oldest->opened) oldest = it;
        }
        if (oldest != items_.end()) items_.erase(oldest);
    }
    return *find(item.id);
}

void Library::touch(const std::string& id) {
    if (Item* it = find(id)) {
        it->opened = time(nullptr);
        dirty_ = true;
    }
}

void Library::set_position(const std::string& id, const Position& p, float progress) {
    Item* it = find(id);
    if (!it) return;
    it->position = p;
    it->progress = std::max(0.f, std::min(1.f, progress));
    if (it->progress >= 0.995f) it->finished = true;
    dirty_ = true;
}

void Library::set_favourite(const std::string& id, bool favourite) {
    if (Item* it = find(id)) {
        it->favourite = favourite;
        dirty_ = true;
    }
}

void Library::add_bookmark(const std::string& id, const Bookmark& b) {
    if (Item* it = find(id)) {
        it->bookmarks.push_back(b);
        dirty_ = true;
    }
}

void Library::remove_bookmark(const std::string& id, size_t index) {
    Item* it = find(id);
    if (it && index < it->bookmarks.size()) {
        it->bookmarks.erase(it->bookmarks.begin() + index);
        dirty_ = true;
    }
}

void Library::remove(const std::string& id) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& it) { return it.id == id; }), items_.end());
    dirty_ = true;
}

std::vector<const Item*> Library::continue_reading(size_t limit) const {
    std::vector<const Item*> out;
    for (const Item& it : items_)
        if (it.kind == ItemKind::Book && it.opened && !it.finished) out.push_back(&it);
    std::sort(out.begin(), out.end(), [](const Item* a, const Item* b) { return a->opened > b->opened; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<const Item*> Library::recent_articles(size_t limit, const std::string& collection) const {
    std::vector<const Item*> out;
    for (const Item& it : items_)
        if (it.kind == ItemKind::Article && it.opened && (collection.empty() || it.collection == collection))
            out.push_back(&it);
    std::sort(out.begin(), out.end(), [](const Item* a, const Item* b) { return a->opened > b->opened; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<const Item*> Library::favourites() const {
    std::vector<const Item*> out;
    for (const Item& it : items_)
        if (it.favourite) out.push_back(&it);
    std::sort(out.begin(), out.end(), [](const Item* a, const Item* b) { return a->opened > b->opened; });
    return out;
}

std::vector<const Item*> Library::article_bookmarks(const std::string& collection) const {
    std::vector<const Item*> out;
    for (const Item& it : items_)
        if (it.kind == ItemKind::Article && (it.favourite || !it.bookmarks.empty()) &&
            (collection.empty() || it.collection == collection))
            out.push_back(&it);
    std::sort(out.begin(), out.end(), [](const Item* a, const Item* b) { return a->opened > b->opened; });
    return out;
}

}  // namespace svc
