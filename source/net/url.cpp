// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "net/url.h"

#include <stdlib.h>

#include <vector>

#include "core/text.h"

namespace lib {

std::string Url::origin() const {
    const bool default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
    return scheme + "://" + host + (default_port ? "" : ":" + std::to_string(port));
}

std::string Url::str() const { return origin() + path + (fragment.empty() ? "" : "#" + fragment); }

bool parse_url(const std::string& text, Url& out) {
    const size_t scheme_end = text.find("://");
    if (scheme_end == std::string::npos) return false;
    out = Url();
    out.scheme = lower(text.substr(0, scheme_end));
    if (out.scheme != "http" && out.scheme != "https") return false;
    size_t host_start = scheme_end + 3;
    size_t path_start = text.find_first_of("/?#", host_start);
    std::string authority = text.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    // Credentials in the URL are not supported; drop them rather than leak them into logs.
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = atoi(authority.c_str() + colon + 1);
    } else {
        out.host = authority;
    }
    if (out.port <= 0) out.port = out.scheme == "https" ? 443 : 80;
    if (out.host.empty()) return false;
    std::string rest = path_start == std::string::npos ? "/" : text.substr(path_start);
    const size_t hash = rest.find('#');
    if (hash != std::string::npos) {
        out.fragment = rest.substr(hash + 1);
        rest.resize(hash);
    }
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    out.path = rest;
    return true;
}

static std::string normalise_path(const std::string& path) {
    // Splits off the query, removes "." and ".." segments, and puts the query back.
    const size_t q = path.find('?');
    const std::string query = q == std::string::npos ? "" : path.substr(q);
    const std::string p = q == std::string::npos ? path : path.substr(0, q);
    std::vector<std::string> out;
    const auto parts = split(p, '/');
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& seg = parts[i];
        if (seg == ".") {
            if (i + 1 == parts.size()) out.push_back("");
        } else if (seg == "..") {
            if (out.size() > 1) out.pop_back();
            if (i + 1 == parts.size()) out.push_back("");
        } else if (seg.empty() && i != 0 && i + 1 != parts.size()) {
            // Keep empty segments: Kiwix's "-/mw///meta..." paths rely on them.
            out.push_back(seg);
        } else {
            out.push_back(seg);
        }
    }
    std::string result;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) result += '/';
        result += out[i];
    }
    if (result.empty() || result[0] != '/') result = "/" + result;
    return result + query;
}

std::string resolve_url(const std::string& base, const std::string& ref) {
    if (ref.empty()) return strip_fragment(base);
    if (ref.find("://") != std::string::npos && ref.find("://") < ref.find_first_of("/?#")) return ref;
    Url b;
    if (!parse_url(base, b)) return ref;
    if (starts_with(ref, "//")) return b.scheme + ":" + ref;
    if (ref[0] == '#') {
        b.fragment = ref.substr(1);
        return b.str();
    }
    std::string fragment;
    std::string r = ref;
    const size_t hash = r.find('#');
    if (hash != std::string::npos) {
        fragment = r.substr(hash + 1);
        r.resize(hash);
    }
    std::string path;
    if (r.empty()) {
        path = b.path;
    } else if (r[0] == '/') {
        path = r;
    } else if (r[0] == '?') {
        path = url_path_only(b.path) + r;
    } else {
        const std::string base_path = url_path_only(b.path);
        path = base_path.substr(0, base_path.rfind('/') + 1) + r;
    }
    b.path = normalise_path(path);
    b.fragment = fragment;
    return b.str();
}

std::string normalise_server_url(const std::string& text) {
    std::string s = trim(text);
    if (s.empty()) return s;
    if (s.find("://") == std::string::npos) s = "http://" + s;
    while (s.size() > 8 && s.back() == '/') s.pop_back();
    return s;
}

bool same_origin(const std::string& a, const std::string& b) {
    Url ua, ub;
    return parse_url(a, ua) && parse_url(b, ub) && ua.scheme == ub.scheme && lower(ua.host) == lower(ub.host) &&
           ua.port == ub.port;
}

std::string url_path_only(const std::string& url) {
    const size_t end = url.find_first_of("?#");
    return end == std::string::npos ? url : url.substr(0, end);
}

std::string strip_fragment(const std::string& url) {
    const size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

}  // namespace lib
