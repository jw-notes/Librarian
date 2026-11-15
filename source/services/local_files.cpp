// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/local_files.h"

#include <algorithm>
#include <functional>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "platform/platform.h"
#include "services/settings.h"

namespace svc {

FileKind kind_of(const std::string& ext) {
    if (ext == "pdf") return FileKind::Pdf;
    if (ext == "cbz" || ext == "cbr" || ext == "cb7" || ext == "zip" || ext == "rar" || ext == "7z") return FileKind::Comic;
    if (ext == "txt" || ext == "md" || ext == "markdown") return FileKind::Text;
    if (ext == "zim") return FileKind::Collection;
    if (ext == "csv" || ext == "tsv" || ext == "tab") return FileKind::Table;
    return FileKind::Book;
}

std::string format_of(const std::string& path) {
    if (lib::ends_with(lib::lower(path), ".fb2.zip")) return "fb2";
    // DAISY talking books: a DAISY 2.02 book's ncc.html, a DAISY 3 package.
    const std::string name = lib::lower(lib::file_name(path));
    if (name == "ncc.html" || name == "ncc.htm" || lib::ends_with(name, ".opf")) return "daisy";
    const std::string ext = lib::file_extension(path);
    if (ext == "fbz") return "fb2";
    if (ext == "azw" || ext == "prc") return "mobi";
    return ext;
}

bool readable_extension(const std::string& ext) {
    return ext == "mobi" || ext == "azw3" || ext == "fb2" || ext == "azw" || ext == "prc" || ext == "fbz" || ext == "epub" || ext == "pdf" || ext == "cbz" || ext == "cbr" || ext == "cb7" || ext == "txt" || ext == "md" || ext == "markdown" ||
           ext == "html" || ext == "htm" || ext == "xhtml" || ext == "docx" || ext == "odt" || ext == "rtf" ||
           ext == "zim" || ext == "csv" || ext == "tsv" || ext == "tab" || ext == "daisy";
}

std::string downloads_dir() { return lib::join_path(platform::data_root(), "books"); }

std::string first_connected_usb() {
    for (const std::string& slot : platform::usb_mount_points()) {
        std::vector<lib::DirEntry> entries;
        if (lib::list_dir(slot, entries, false) && !entries.empty()) return slot;
    }
    return "";
}

std::string preferred_download_dir() {
    if (settings().download_to_usb) {
        const std::string usb = first_connected_usb();
        if (!usb.empty()) return lib::join_path(usb, "Librarian");
    }
    return downloads_dir();
}

std::string title_from_file(const std::string& path) {
    std::string t = lib::file_stem(path);
    for (char& c : t)
        if (c == '_') c = ' ';
    return lib::collapse_space(t);
}

}  // namespace svc
