// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/openers.h"

#include <time.h>

#include "core/fs.h"
#include "ui/kiwix_screens.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/covers.h"
#include "platform/platform.h"
#include "services/local_files.h"
#include "services/local_library.h"
#include "ui/app.h"
#include "ui/book_sources.h"
#include "ui/reader.h"

namespace ui {

bool is_supported_file(const std::string& path) {
    const std::string ext = svc::format_of(path);
    return ext == "mobi" || ext == "azw3" || ext == "fb2" || ext == "epub" || ext == "txt" || ext == "pdf" || ext == "cbz" || ext == "cbr" || ext == "zip" || ext == "rar" ||
           ext == "cb7" || ext == "7z" || ext == "html" || ext == "htm" || ext == "xhtml" || ext == "md" || ext == "markdown" ||
           ext == "docx" || ext == "odt" || ext == "rtf" || ext == "csv" || ext == "tsv" || ext == "tab" || ext == "daisy";
}

void open_file(const std::string& path, const std::string& title, const std::string& author, const std::string& cover,
               const std::string& source, const std::string& remote_id) {
    if (!lib::file_exists(path)) {
        app().toast("The file is missing: " + lib::file_name(path));
        return;
    }
    if (svc::format_of(path) == "zim") {
        open_zim_file(path);
        return;
    }
    const std::string fingerprint = svc::file_fingerprint(path, lib::file_size(path));
    // A book read before at another path (a USB drive in another slot, a moved
    // file) is the same item: its position, bookmarks and favourite carry on.
    if (svc::Item* existing = svc::library().find_file(path, fingerprint)) {
        if (existing->path != path) {
            LOG("local: %s is now at %s", existing->path.c_str(), path.c_str());
            existing->cover = doc::relocate_cover(existing->cover, existing->path, path);
            existing->path = path;
        }
        existing->fingerprint = fingerprint;
        if (existing->title.empty()) existing->title = title.empty() ? lib::file_stem(path) : title;
        if (existing->author.empty()) existing->author = author;
        svc::library().mark_dirty();
        svc::library().touch(existing->id);
        const svc::Item copy = *existing;
        open_book_file(copy);
        return;
    }
    svc::Item item;
    item.id = svc::file_id(path);
    item.fingerprint = fingerprint;
    item.kind = svc::ItemKind::Book;
    item.source = source;
    item.title = title.empty() ? lib::file_stem(path) : title;
    item.author = author;
    item.path = path;
    item.cover = cover;
    item.format = svc::format_of(path);
    item.remote_id = remote_id;
    svc::library().upsert(item);
    svc::library().touch(item.id);
    open_book_file(item);
}

void open_item(const svc::Item& item) {
    if (item.kind == svc::ItemKind::Article) {
        open_article(item.url, item.collection_title);
        return;
    }
    if (!lib::file_exists(item.path)) {
        // Look for the same book elsewhere: the drive may be in another slot.
        const svc::LocalDoc* found = nullptr;
        if (!item.fingerprint.empty()) found = svc::local_library().find_available(item.fingerprint);
        if (found) {
            open_file(found->path, item.title, item.author, item.cover, item.source, item.remote_id);
            return;
        }
        bool on_usb = false;
        for (const std::string& slot : platform::usb_mount_points())
            if (lib::starts_with(item.path, slot + "/")) on_usb = true;
        app().toast(on_usb ? "This book is on a USB drive that isn't connected. Plug it in to carry on reading."
                           : "This book's file is no longer on the console.");
        return;
    }
    svc::library().touch(item.id);
    open_book_file(item);
}

}  // namespace ui
