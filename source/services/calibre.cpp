// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/calibre.h"

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "net/url.h"
#include "services/local_files.h"
#include "services/settings.h"

namespace svc::calibre {

std::string server() { return lib::normalise_server_url(settings().calibre_url); }

namespace {
std::string g_search_template;

std::string host_of(const std::string& url) {
    lib::Url u;
    return lib::parse_url(url, u) ? u.host : url;
}
}  // namespace

std::vector<Catalog> catalogs() {
    std::vector<Catalog> out;
    if (!server().empty()) out.push_back({"Calibre-Web", server() + "/opds", true});
    for (const auto& c : settings().opds_catalogs)
        out.push_back({c.name.empty() ? host_of(c.url) : c.name, c.url, false});
    return out;
}

// Remembered by address (in settings.json), so it stays the same when other
// catalogues are added or removed, and across restarts.
int current_catalog() {
    const auto all = catalogs();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].url == settings().books_catalog) return (int)i;
    return 0;
}

void set_current_catalog(int index) {
    const auto all = catalogs();
    if (index < 0 || index >= (int)all.size()) return;
    if (index != current_catalog()) g_search_template.clear();
    if (settings().books_catalog != all[index].url) {
        settings().books_catalog = all[index].url;
        settings_save();
    }
}

std::string root_url() {
    const auto all = catalogs();
    return all.empty() ? std::string() : all[current_catalog()].url;
}

std::string search_url(const std::string& query) {
    const auto all = catalogs();
    if (all.empty()) return std::string();
    if (all[current_catalog()].calibre) return server() + "/opds/search?query=" + lib::url_encode(query);
    return g_search_template.empty() ? std::string() : opds_search_url(g_search_template, query);
}

void set_search_template(const std::string& t) { g_search_template = t; }

bool readable_format(const std::string& f) {
    return f == "azw3" || f == "mobi" || f == "fb2" || f == "epub" || f == "pdf" || f == "cbz" || f == "cbr" || f == "cb7" || f == "txt" || f == "html" ||
           f == "docx" || f == "odt" || f == "rtf";
}

const OpdsAcquisition* preferred_format(const OpdsEntry& entry) {
    for (const auto& want : lib::split(settings().format_order, ','))
        for (const auto& f : entry.formats)
            if (f.format == lib::trim(want)) return &f;
    for (const auto& f : entry.formats)
        if (readable_format(f.format)) return &f;
    return nullptr;
}

std::string download_path(const OpdsEntry& entry, const std::string& format) {
    // "Title - Author [id].epub": readable in a file manager, unique per book.
    std::string name = entry.title;
    if (!entry.author.empty()) name += " - " + entry.author;
    name = lib::safe_file_name(name);
    if (!entry.book_id.empty()) name += " [" + entry.book_id + "]";
    return lib::join_path(preferred_download_dir(), name + "." + format);
}

bool download(const OpdsEntry& entry, const OpdsAcquisition& format, const lib::Progress& progress, std::string& path,
              std::string& error) {
    lib::make_dirs(preferred_download_dir());
    path = download_path(entry, format.format);
    lib::HttpRequest request;
    request.url = format.href;
    request.timeout_seconds = 60;
    lib::HttpResponse r = lib::http_download(request, path, progress);
    if (!r.ok()) {
        error = lib::describe_failure(r);
        LOG("calibre: download failed: %s", error.c_str());
        return false;
    }
    LOG("calibre: downloaded %s (%s)", lib::file_name(path).c_str(), lib::format_bytes(lib::file_size(path)).c_str());
    return true;
}

}  // namespace svc::calibre
