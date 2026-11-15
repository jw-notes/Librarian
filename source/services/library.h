// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// The reading database (library.json): every book or article that has been
// opened, with its reading position, bookmarks and favourite flag. It drives
// Home's Continue Reading and Recent Articles rows and lets books open where
// they were left, with or without a server.
#include <stdint.h>

#include <string>
#include <vector>

namespace svc {

enum class ItemKind { Book, Article };

struct Position {
    int section = 0;    // chapter (EPUB) or page (comic, PDF)
    int block = 0;      // block within the section's document
    float offset = 0;   // fraction through that block, 0..1
};

struct Bookmark {
    Position at;
    std::string label;
    int64_t created = 0;
};

struct Item {
    std::string id;       // "file:/path", "kiwix:<book>/<path>"
    ItemKind kind = ItemKind::Book;
    std::string source;   // "local", "calibre", "kiwix"
    std::string title, author;
    std::string path;     // local file (books)
    std::string url;      // article address (Kiwix)
    std::string collection, collection_title;  // Kiwix book name and title
    std::string cover;    // URL or path
    std::string format;   // epub, pdf, cbz, cbr, txt, html
    std::string remote_id;  // Calibre book id
    // Local files: "name|size", which stays the same when a USB drive comes back
    // in another slot or the file is moved to another folder.
    std::string fingerprint;
    int64_t added = 0, opened = 0;
    Position position;
    float progress = 0;   // 0..1
    bool finished = false;
    bool favourite = false;
    std::vector<Bookmark> bookmarks;
};

class Library {
public:
    void load();
    // Saves if anything changed (cheap to call often).
    void save_if_dirty();

    Item* find(const std::string& id);
    // The item for a local file: by its path, or else by its fingerprint (the
    // same book seen before at another path).
    Item* find_file(const std::string& path, const std::string& fingerprint);
    // Returns the item with this id, creating it if needed.
    Item& upsert(const Item& item);
    void touch(const std::string& id);  // opened now
    void set_position(const std::string& id, const Position& p, float progress);
    void set_favourite(const std::string& id, bool favourite);
    void add_bookmark(const std::string& id, const Bookmark& b);
    void remove_bookmark(const std::string& id, size_t index);
    void remove(const std::string& id);
    void mark_dirty() { dirty_ = true; }

    // Books with a position, most recent first.
    std::vector<const Item*> continue_reading(size_t limit) const;
    std::vector<const Item*> recent_articles(size_t limit, const std::string& collection = "") const;
    std::vector<const Item*> favourites() const;
    std::vector<const Item*> article_bookmarks(const std::string& collection = "") const;
    const std::vector<Item>& items() const { return items_; }

private:
    std::vector<Item> items_;
    bool dirty_ = false;
};

Library& library();
std::string article_id(const std::string& collection, const std::string& path);
std::string file_id(const std::string& path);
// "alice.epub|189249": file name (lower case) and size.
std::string file_fingerprint(const std::string& path, uint64_t size);

}  // namespace svc
