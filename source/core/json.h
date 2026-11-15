// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// A small JSON value type with a parser and a writer, used for settings, the
// reading database and Kiwix's suggestion API.
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lib {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool b) : type_(Type::Bool), number_(b) {}
    Json(int n) : type_(Type::Number), number_(n) {}
    Json(int64_t n) : type_(Type::Number), number_((double)n) {}
    Json(double n) : type_(Type::Number), number_(n) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}
    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    // Returns Null on malformed input.
    static Json parse(const std::string& text, bool* ok = nullptr);
    std::string dump(bool pretty = false) const;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    // Lenient accessors: a missing or mistyped value gives the fallback.
    std::string str(const std::string& fallback = "") const { return type_ == Type::String ? string_ : fallback; }
    double num(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    int64_t integer(int64_t fallback = 0) const { return type_ == Type::Number ? (int64_t)number_ : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? number_ != 0 : fallback; }

    const Json& operator[](const std::string& key) const;
    const Json& operator[](size_t index) const;
    Json& set(const std::string& key, Json value);
    void push(Json value);
    void erase(const std::string& key);
    bool has(const std::string& key) const;
    size_t size() const;
    const std::vector<Json>& items() const { return array_; }
    std::vector<Json>& items() { return array_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return object_; }

private:
    void write(std::string& out, bool pretty, int depth) const;
    Type type_ = Type::Null;
    double number_ = 0;
    std::string string_;
    std::vector<Json> array_;
    // Insertion-ordered so saved files stay stable and readable.
    std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace lib
