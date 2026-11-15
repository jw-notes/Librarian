// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// String helpers shared by every module: UTF-8, case, trimming, HTML entities,
// URL and Base64 encoding.
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace lib {

// Decodes one code point at `i` and advances past it. Invalid bytes decode as U+FFFD.
uint32_t utf8_next(std::string_view s, size_t& i);
void utf8_append(std::string& out, uint32_t cp);
// Byte length of the longest prefix of `s` holding at most `count` code points.
size_t utf8_prefix(std::string_view s, size_t count);
// Converts Latin-1 / Windows-1252 bytes to UTF-8 when `s` is not valid UTF-8.
std::string to_utf8(std::string_view s);
bool is_valid_utf8(std::string_view s);

std::string lower(std::string_view s);
std::string trim(std::string_view s);
// Collapses runs of whitespace to single spaces and trims the ends.
std::string collapse_space(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);
bool icontains(std::string_view haystack, std::string_view needle);
std::vector<std::string> split(std::string_view s, char separator);
std::string replace_all(std::string s, std::string_view from, std::string_view to);
// "file name 10" sorts after "file name 9".
bool natural_less(std::string_view a, std::string_view b);

// &amp; &#233; &eacute; ... -> UTF-8.
std::string decode_entities(std::string_view s);
std::string escape_xml(std::string_view s);
// Removes tags from a fragment of HTML and decodes its entities.
std::string strip_tags(std::string_view html);

std::string url_encode(std::string_view s, bool keep_slash = false);
std::string url_decode(std::string_view s);
std::string base64_encode(std::string_view s);

std::string format_bytes(uint64_t bytes);
std::string format_count(uint64_t n);  // 91828 -> "91,828"

}  // namespace lib
