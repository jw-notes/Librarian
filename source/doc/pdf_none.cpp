// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Builds without MuPDF: PDF files are reported as unsupported.
#include "doc/paged.h"

namespace doc {

std::shared_ptr<PagedDocument> open_pdf(const std::string& path, std::string& error, int) {
    (void)path;
    error = "PDF support is not included in this build of Librarian.";
    return nullptr;
}

}  // namespace doc
