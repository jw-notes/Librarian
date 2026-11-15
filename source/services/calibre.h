// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Calibre-Web on top of OPDS: addresses, format choice and downloads.
#include <string>
#include <vector>

#include "net/http.h"
#include "services/opds.h"

namespace svc {
namespace calibre {

std::string server();
// The catalogues the Books section shows: Calibre-Web first (when its address
// is set), then those added in Settings › Books. One is current.
struct Catalog {
    std::string name, url;  // url: the catalogue's root feed
    bool calibre = false;
};
std::vector<Catalog> catalogs();
int current_catalog();  // index into catalogs()
void set_current_catalog(int index);
// The current catalogue's root feed, and a search in it ("" when it has none).
std::string root_url();
std::string search_url(const std::string& query);
// Remembered when the root feed of the current catalogue is read (not needed
// for Calibre-Web, whose search address is known).
void set_search_template(const std::string& search_template);
// The format to download, from the user's preference order; empty if none of
// the entry's formats can be read.
const OpdsAcquisition* preferred_format(const OpdsEntry& entry);
bool readable_format(const std::string& format);
// Where a downloaded copy of this entry in this format lives.
std::string download_path(const OpdsEntry& entry, const std::string& format);
// Downloads into the library and records the item. Runs on a worker.
bool download(const OpdsEntry& entry, const OpdsAcquisition& format, const lib::Progress& progress, std::string& path,
              std::string& error);

}  // namespace calibre
}  // namespace svc
