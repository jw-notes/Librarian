// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Finds readable documents in Librarian's download folder, /data/books and
// USB drives.
#include <stdint.h>

#include <string>
#include <vector>

namespace svc {

// Collection: a ZIM file (Kiwix), opened in the Knowledge screens.
enum class FileKind { Book, Comic, Pdf, Text, Collection, Table };


std::string format_of(const std::string& path);
FileKind kind_of(const std::string& ext);
bool readable_extension(const std::string& ext);
// Where Calibre-Web downloads go.
std::string downloads_dir();
// Where new downloads are saved, honouring the "download to USB" setting:
// a connected USB drive's "Librarian" folder, or console storage.
std::string preferred_download_dir();
// The first connected USB drive's mount point, or "" when none is connected.
std::string first_connected_usb();
// A friendlier title from a file name: "the_hobbit-v2" -> "the hobbit-v2".
std::string title_from_file(const std::string& path);

}  // namespace svc
