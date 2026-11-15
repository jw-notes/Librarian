// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// A small CommonMark-ish Markdown to HTML converter, enough for README and
// documentation files: headings, emphasis, inline code, fenced and indented
// code, lists (nested), blockquotes, links, images, horizontal rules, pipe
// tables and hard line breaks. The HTML is then rendered by the shared
// html_to_document converter, so the reader handles the rest.
#include <string>

namespace doc {

std::string markdown_to_html(const std::string& markdown);

}  // namespace doc
