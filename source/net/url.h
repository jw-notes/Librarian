// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
#include <string>

namespace lib {

struct Url {
    std::string scheme;  // "http" / "https"
    std::string host;
    int port = 0;
    std::string path = "/";  // path + query, without the fragment
    std::string fragment;

    std::string origin() const;  // scheme://host[:port]
    std::string str() const;
};

bool parse_url(const std::string& text, Url& out);
// Resolves `ref` (absolute, root-relative, relative, "../x", "?q", "#f") against `base`.
std::string resolve_url(const std::string& base, const std::string& ref);
// "192.168.1.10:8080" -> "http://192.168.1.10:8080"; strips a trailing slash.
std::string normalise_server_url(const std::string& text);
bool same_origin(const std::string& a, const std::string& b);
// The path without query or fragment, still percent-encoded.
std::string url_path_only(const std::string& url);
// Removes "#fragment".
std::string strip_fragment(const std::string& url);

}  // namespace lib
