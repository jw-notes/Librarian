// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "core/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/text.h"

namespace lib {

namespace {

const Json kNull;

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    int depth = 0;

    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eat(char c) {
        space();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }
    bool literal(const char* word) {
        size_t n = strlen(word);
        if (s.compare(i, n, word) == 0) { i += n; return true; }
        return false;
    }
    unsigned hex4() {
        if (i + 4 > s.size()) { ok = false; return 0; }
        unsigned v = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
        i += 4;
        return v;
    }
    std::string string() {
        std::string out;
        ++i;  // opening quote
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            c = s[i++];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                        i += 2;
                        const uint32_t low = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    utf8_append(out, cp);
                    break;
                }
                default: out += c;
            }
        }
        if (i >= s.size()) ok = false;
        ++i;  // closing quote
        return out;
    }
    Json value() {
        space();
        if (i >= s.size() || ++depth > 64) { ok = false; return Json(); }
        Json result;
        const char c = s[i];
        if (c == '{') {
            ++i;
            result = Json::object();
            if (!eat('}')) {
                do {
                    space();
                    if (i >= s.size() || s[i] != '"') { ok = false; break; }
                    std::string key = string();
                    if (!eat(':')) { ok = false; break; }
                    result.set(key, value());
                } while (ok && eat(','));
                if (ok && !eat('}')) ok = false;
            }
        } else if (c == '[') {
            ++i;
            result = Json::array();
            if (!eat(']')) {
                do result.push(value());
                while (ok && eat(','));
                if (ok && !eat(']')) ok = false;
            }
        } else if (c == '"') {
            result = Json(string());
        } else if (literal("true")) {
            result = Json(true);
        } else if (literal("false")) {
            result = Json(false);
        } else if (literal("null")) {
        } else {
            char* end = nullptr;
            const double d = strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) ok = false;
            else { i = end - s.c_str(); result = Json(d); }
        }
        --depth;
        return result;
    }
};

void write_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

}  // namespace

Json Json::parse(const std::string& text, bool* ok) {
    Parser p{text};
    Json result = p.value();
    p.space();
    if (p.i != text.size()) p.ok = false;
    if (ok) *ok = p.ok;
    return p.ok ? result : Json();
}

const Json& Json::operator[](const std::string& key) const {
    for (const auto& [k, v] : object_)
        if (k == key) return v;
    return kNull;
}

const Json& Json::operator[](size_t index) const { return index < array_.size() ? array_[index] : kNull; }

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) { *this = object(); }
    for (auto& [k, v] : object_)
        if (k == key) { v = std::move(value); return v; }
    object_.emplace_back(key, std::move(value));
    return object_.back().second;
}

void Json::push(Json value) {
    if (type_ != Type::Array) *this = array();
    array_.push_back(std::move(value));
}

void Json::erase(const std::string& key) {
    for (auto it = object_.begin(); it != object_.end(); ++it)
        if (it->first == key) { object_.erase(it); return; }
}

bool Json::has(const std::string& key) const {
    for (const auto& [k, v] : object_)
        if (k == key) return true;
    return false;
}

size_t Json::size() const { return type_ == Type::Array ? array_.size() : type_ == Type::Object ? object_.size() : 0; }

std::string Json::dump(bool pretty) const {
    std::string out;
    write(out, pretty, 0);
    return out;
}

void Json::write(std::string& out, bool pretty, int depth) const {
    auto newline = [&](int d) {
        if (!pretty) return;
        out += '\n';
        out.append(d * 2, ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += number_ ? "true" : "false"; break;
        case Type::Number: {
            char buffer[32];
            if (number_ == floor(number_) && fabs(number_) < 1e15) snprintf(buffer, sizeof buffer, "%lld", (long long)number_);
            else snprintf(buffer, sizeof buffer, "%.9g", number_);
            out += buffer;
            break;
        }
        case Type::String: write_string(out, string_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                array_[i].write(out, pretty, depth + 1);
            }
            if (!array_.empty()) newline(depth);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                write_string(out, object_[i].first);
                out += pretty ? ": " : ":";
                object_[i].second.write(out, pretty, depth + 1);
            }
            if (!object_.empty()) newline(depth);
            out += '}';
            break;
    }
}

}  // namespace lib
