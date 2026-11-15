// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// A forgiving HTML / XML tree builder. It never fails: malformed markup gives
// the best tree it can. HTML mode knows void elements, raw-text elements and
// the common implied end tags (<p>, <li>, <td> ...); XML mode only honours
// explicit structure. Entities are decoded in text and attribute values.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lib {

struct Node {
    std::string tag;   // lower-case; empty for a text node
    std::string text;  // text nodes only
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;

    bool is_text() const { return tag.empty(); }
    // The tag without its namespace prefix ("dc:title" -> "title").
    std::string_view local() const;
    const std::string& attr(std::string_view name) const;
    bool has_attr(std::string_view name) const;
    bool has_class(std::string_view name) const;

    // First descendant (depth-first) whose local name matches.
    const Node* find(std::string_view local_name) const;
    void find_all(std::string_view local_name, std::vector<const Node*>& out) const;
    // Direct children with this local name.
    std::vector<const Node*> elements(std::string_view local_name) const;
    const Node* child(std::string_view local_name) const;
    // Concatenated text of all descendants, whitespace collapsed.
    std::string inner_text() const;
    // Text of the first child element with this local name, whitespace collapsed.
    std::string child_text(std::string_view local_name) const;
    // Re-serialises the children (used for OPDS <content type="xhtml">).
    std::string inner_markup() const;
};

enum class MarkupMode { Html, Xml };

std::unique_ptr<Node> parse_markup(std::string_view source, MarkupMode mode);

}  // namespace lib
