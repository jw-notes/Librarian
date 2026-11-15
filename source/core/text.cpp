// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "core/text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace lib {

uint32_t utf8_next(std::string_view s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = c & (0x3F >> extra);
    for (int n = 0; n < extra; ++n) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

void utf8_append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

size_t utf8_prefix(std::string_view s, size_t count) {
    size_t i = 0;
    while (i < s.size() && count--) utf8_next(s, i);
    return i;
}

bool is_valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        if (utf8_next(s, i) == 0xFFFD) return false;
    }
    return true;
}

std::string to_utf8(std::string_view s) {
    if (is_valid_utf8(s)) return std::string(s);
    // Windows-1252's 0x80..0x9F block; everything else maps straight to Latin-1.
    static const uint16_t cp1252[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (unsigned char c : s) utf8_append(out, c >= 0x80 && c < 0xA0 ? cp1252[c - 0x80] : c);
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::string collapse_space(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (isspace((unsigned char)c)) {
            space = !out.empty();
        } else {
            if (space) out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != haystack.end();
}

std::vector<std::string> split(std::string_view s, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == separator) {
            parts.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

bool natural_less(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == '0') ++i2;
            while (j2 < b.size() && b[j2] == '0') ++j2;
            size_t ie = i2, je = j2;
            while (ie < a.size() && isdigit((unsigned char)a[ie])) ++ie;
            while (je < b.size() && isdigit((unsigned char)b[je])) ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2;
            const int cmp = a.substr(i2, ie - i2).compare(b.substr(j2, je - j2));
            if (cmp) return cmp < 0;
            i = ie;
            j = je;
        } else {
            const int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[j]);
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

namespace {
struct Entity { const char* name; uint32_t cp; };
// The named entities seen in practice in Wikipedia/Kiwix pages and EPUB XHTML.
const Entity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"hellip", 0x2026}, {"bull", 0x2022},
    {"middot", 0xB7}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"deg", 0xB0},
    {"plusmn", 0xB1}, {"times", 0xD7}, {"divide", 0xF7}, {"minus", 0x2212}, {"frac12", 0xBD},
    {"frac14", 0xBC}, {"frac34", 0xBE}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"micro", 0xB5},
    {"para", 0xB6}, {"sect", 0xA7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsaquo", 0x2039},
    {"rsaquo", 0x203A}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
    {"iexcl", 0xA1}, {"iquest", 0xBF}, {"shy", 0xAD}, {"zwj", 0x200D}, {"zwnj", 0x200C},
    {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"dagger", 0x2020}, {"Dagger", 0x2021},
    {"prime", 0x2032}, {"Prime", 0x2033}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191},
    {"darr", 0x2193}, {"harr", 0x2194}, {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260},
    {"asymp", 0x2248}, {"infin", 0x221E}, {"sum", 0x2211}, {"radic", 0x221A}, {"pi", 0x3C0},
    {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"mu", 0x3BC},
    {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4},
    {"Aring", 0xC5}, {"AElig", 0xC6}, {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9},
    {"Ecirc", 0xCA}, {"Euml", 0xCB}, {"Iacute", 0xCD}, {"Ntilde", 0xD1}, {"Oacute", 0xD3},
    {"Ouml", 0xD6}, {"Oslash", 0xD8}, {"Uacute", 0xDA}, {"Uuml", 0xDC}, {"szlig", 0xDF},
    {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4},
    {"aring", 0xE5}, {"aelig", 0xE6}, {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9},
    {"ecirc", 0xEA}, {"euml", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE},
    {"iuml", 0xEF}, {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4},
    {"otilde", 0xF5}, {"ouml", 0xF6}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA},
    {"ucirc", 0xFB}, {"uuml", 0xFC}, {"yacute", 0xFD}, {"yuml", 0xFF}, {"oelig", 0x153},
    {"OElig", 0x152}, {"scaron", 0x161}, {"Scaron", 0x160},
};
}  // namespace

std::string decode_entities(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') { out += s[i++]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 12) { out += s[i++]; continue; }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            cp = (uint32_t)strtoul(std::string(name.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
        } else {
            for (const auto& e : kEntities)
                if (name == e.name) { cp = e.cp; break; }
        }
        if (cp == 0 || cp > 0x10FFFF) { out += s[i++]; continue; }
        utf8_append(out, cp);
        i = semi + 1;
    }
    return out;
}

std::string escape_xml(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string strip_tags(std::string_view html) {
    std::string out;
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>' && in_tag) in_tag = false;
        else if (!in_tag) out += c;
    }
    return collapse_space(decode_entities(out));
}

std::string url_encode(std::string_view s, bool keep_slash) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string url_decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string base64_encode(std::string_view s) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < s.size(); i += 3) {
        const uint32_t v = ((uint8_t)s[i] << 16) | ((uint8_t)s[i + 1] << 8) | (uint8_t)s[i + 2];
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < s.size()) {
        uint32_t v = (uint8_t)s[i] << 16;
        if (i + 1 < s.size()) v |= (uint8_t)s[i + 1] << 8;
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        out += i + 1 < s.size() ? table[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string format_bytes(uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024ull * 1024 * 1024) snprintf(buffer, sizeof buffer, "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1024 * 1024) snprintf(buffer, sizeof buffer, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) snprintf(buffer, sizeof buffer, "%llu KB", (unsigned long long)(bytes / 1024));
    else snprintf(buffer, sizeof buffer, "%llu B", (unsigned long long)bytes);
    return buffer;
}

std::string format_count(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

}  // namespace lib
