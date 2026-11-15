// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <functional>
#include <set>
#include <string>

#include "core/markup.h"
#include "doc/document.h"

namespace doc {

struct ConvertOptions {
    // Resolves an href/src found in the page to the address stored in the
    // document (a URL for Kiwix, an archive path for EPUB).
    std::function<std::string(const std::string&)> resolve;
    // Optional: intrinsic size of an image the page gives no size for.
    std::function<bool(const std::string& src, int& w, int& h)> image_size;
    // Skip images smaller than this in both directions (icons, flags, spacers).
    int min_image = 24;
    // Read-along: elements with these ids become marks (Document::marks).
    std::set<std::string> mark_ids;
};

// Converts a parsed HTML/XHTML tree. Appends to `out` (an EPUB book is several
// chapters, each converted separately).
void html_to_document(const lib::Node& root, const ConvertOptions& options, Document& out);
Document html_to_document(const std::string& html, const ConvertOptions& options);

}  // namespace doc
