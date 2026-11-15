// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "core/markup.h"

#include <ctype.h>
#include <string.h>

#include "core/text.h"

namespace lib {

namespace {

const std::string kEmpty;

bool one_of(std::string_view tag, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (tag == n) return true;
    return false;
}

bool is_void(std::string_view tag) {
    return one_of(tag, {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
                        "source", "track", "wbr"});
}

// Elements whose start closes an open <p>.
bool closes_paragraph(std::string_view tag) {
    return one_of(tag, {"p", "div", "ul", "ol", "dl", "table", "h1", "h2", "h3", "h4", "h5", "h6", "section",
                        "article", "aside", "blockquote", "pre", "figure", "hr", "header", "footer", "nav",
                        "details", "address", "form", "main"});
}

class Builder {
public:
    Builder(std::string_view s, MarkupMode mode) : s_(s), html_(mode == MarkupMode::Html) {
        root_ = std::make_unique<Node>();
        root_->tag = "#document";
        current_ = root_.get();
    }

    std::unique_ptr<Node> run() {
        while (i_ < s_.size()) {
            if (s_[i_] == '<') tag();
            else text();
        }
        return std::move(root_);
    }

private:
    void text() {
        const size_t end = s_.find('<', i_);
        const size_t stop = end == std::string_view::npos ? s_.size() : end;
        add_text(decode_entities(s_.substr(i_, stop - i_)));
        i_ = stop;
    }

    void add_text(std::string value) {
        if (value.empty()) return;
        if (!current_->children.empty() && current_->children.back()->is_text()) {
            current_->children.back()->text += value;
            return;
        }
        auto node = std::make_unique<Node>();
        node->text = std::move(value);
        node->parent = current_;
        current_->children.push_back(std::move(node));
    }

    void tag() {
        if (s_.compare(i_, 4, "<!--") == 0) { skip_past("-->", 4); return; }
        if (s_.compare(i_, 9, "<![CDATA[") == 0) {
            const size_t end = s_.find("]]>", i_ + 9);
            const size_t stop = end == std::string_view::npos ? s_.size() : end;
            add_text(std::string(s_.substr(i_ + 9, stop - i_ - 9)));
            i_ = stop == s_.size() ? stop : stop + 3;
            return;
        }
        if (s_.compare(i_, 2, "<!") == 0 || s_.compare(i_, 2, "<?") == 0) { skip_past(">", 2); return; }
        if (i_ + 1 < s_.size() && s_[i_ + 1] == '/') { end_tag(); return; }
        if (i_ + 1 >= s_.size() || !(isalpha((unsigned char)s_[i_ + 1]))) {
            add_text("<");
            ++i_;
            return;
        }
        start_tag();
    }

    void skip_past(const char* marker, size_t skip) {
        const size_t end = s_.find(marker, i_ + skip);
        i_ = end == std::string_view::npos ? s_.size() : end + strlen(marker);
    }

    std::string name() {
        const size_t start = i_;
        while (i_ < s_.size() && !isspace((unsigned char)s_[i_]) && s_[i_] != '>' && s_[i_] != '/' && s_[i_] != '=')
            ++i_;
        return lower(s_.substr(start, i_ - start));
    }

    void spaces() {
        while (i_ < s_.size() && isspace((unsigned char)s_[i_])) ++i_;
    }

    void start_tag() {
        ++i_;
        auto node = std::make_unique<Node>();
        node->tag = name();
        bool self_closing = false;
        while (i_ < s_.size()) {
            spaces();
            if (i_ >= s_.size()) break;
            if (s_[i_] == '>') { ++i_; break; }
            if (s_[i_] == '/') { self_closing = true; ++i_; continue; }
            std::string key = name();
            if (key.empty()) { ++i_; continue; }
            std::string value;
            spaces();
            if (i_ < s_.size() && s_[i_] == '=') {
                ++i_;
                spaces();
                if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
                    const char quote = s_[i_++];
                    const size_t end = s_.find(quote, i_);
                    const size_t stop = end == std::string_view::npos ? s_.size() : end;
                    value = decode_entities(s_.substr(i_, stop - i_));
                    i_ = stop == s_.size() ? stop : stop + 1;
                } else {
                    const size_t start = i_;
                    while (i_ < s_.size() && !isspace((unsigned char)s_[i_]) && s_[i_] != '>') ++i_;
                    value = decode_entities(s_.substr(start, i_ - start));
                }
            }
            node->attrs.emplace_back(std::move(key), std::move(value));
        }

        const std::string tag = node->tag;
        if (html_) implied_ends(tag);

        if (html_ && (tag == "script" || tag == "style" || tag == "noscript" || tag == "template")) {
            // Raw text: skipped entirely, nothing in it is rendered.
            if (!self_closing) {
                const std::string close = "</" + tag;
                size_t at = i_;
                for (;;) {
                    at = s_.find("</", at);
                    if (at == std::string_view::npos) { i_ = s_.size(); return; }
                    if (iequals(s_.substr(at, close.size()), close)) break;
                    at += 2;
                }
                const size_t end = s_.find('>', at);
                i_ = end == std::string_view::npos ? s_.size() : end + 1;
            }
            return;
        }

        node->parent = current_;
        Node* raw = node.get();
        current_->children.push_back(std::move(node));
        if (!self_closing && !(html_ && is_void(tag))) current_ = raw;
    }

    void implied_ends(const std::string& tag) {
        auto close_if_open = [&](std::initializer_list<const char*> names, std::initializer_list<const char*> scope) {
            for (Node* n = current_; n && n != root_.get(); n = n->parent) {
                if (one_of(n->tag, names)) { current_ = n->parent; return; }
                if (one_of(n->tag, scope)) return;
            }
        };
        if (closes_paragraph(tag)) close_if_open({"p"}, {"div", "td", "th", "li", "blockquote", "section", "body"});
        if (tag == "li") close_if_open({"li"}, {"ul", "ol", "menu"});
        if (tag == "dt" || tag == "dd") close_if_open({"dt", "dd"}, {"dl"});
        if (tag == "tr") close_if_open({"tr"}, {"table", "tbody", "thead", "tfoot"});
        if (tag == "td" || tag == "th") close_if_open({"td", "th"}, {"tr", "table"});
        if (tag == "tbody" || tag == "thead" || tag == "tfoot") close_if_open({"tbody", "thead", "tfoot"}, {"table"});
        if (tag == "option") close_if_open({"option"}, {"select"});
    }

    void end_tag() {
        i_ += 2;
        const std::string tag = name();
        const size_t end = s_.find('>', i_);
        i_ = end == std::string_view::npos ? s_.size() : end + 1;
        for (Node* n = current_; n && n != root_.get(); n = n->parent) {
            if (n->tag == tag) {
                current_ = n->parent;
                return;
            }
            // In HTML an end tag does not close past a table cell or list boundary
            // it does not belong to; stray end tags are ignored.
            if (html_ && one_of(n->tag, {"td", "th", "table"}) && tag != n->tag) {
                if (!one_of(tag, {"tr", "table", "tbody", "thead", "tfoot"})) return;
            }
        }
    }

    std::string_view s_;
    bool html_;
    size_t i_ = 0;
    std::unique_ptr<Node> root_;
    Node* current_;
};

void collect_text(const Node* node, std::string& out) {
    if (node->is_text()) { out += node->text; return; }
    if (node->tag == "br") out += ' ';
    for (const auto& c : node->children) collect_text(c.get(), out);
}

void serialise(const Node* node, std::string& out) {
    if (node->is_text()) { out += escape_xml(node->text); return; }
    out += '<' + node->tag;
    for (const auto& [k, v] : node->attrs) out += ' ' + k + "=\"" + escape_xml(v) + '"';
    out += '>';
    for (const auto& c : node->children) serialise(c.get(), out);
    out += "</" + node->tag + '>';
}

}  // namespace

std::string_view Node::local() const {
    const size_t colon = tag.find(':');
    return colon == std::string::npos ? std::string_view(tag) : std::string_view(tag).substr(colon + 1);
}

const std::string& Node::attr(std::string_view name) const {
    for (const auto& [k, v] : attrs) {
        if (k == name) return v;
        // Namespaced attributes (xlink:href, opf:role) match on their local name.
        const size_t colon = k.find(':');
        if (colon != std::string::npos && std::string_view(k).substr(colon + 1) == name) return v;
    }
    return kEmpty;
}

bool Node::has_attr(std::string_view name) const {
    for (const auto& [k, v] : attrs)
        if (k == name) return true;
    return false;
}

bool Node::has_class(std::string_view name) const {
    const std::string& classes = attr("class");
    size_t at = 0;
    while ((at = classes.find(name, at)) != std::string::npos) {
        const bool start = at == 0 || isspace((unsigned char)classes[at - 1]);
        const size_t end = at + name.size();
        if (start && (end == classes.size() || isspace((unsigned char)classes[end]))) return true;
        at = end;
    }
    return false;
}

const Node* Node::find(std::string_view local_name) const {
    for (const auto& c : children) {
        if (c->is_text()) continue;
        if (c->local() == local_name) return c.get();
        if (const Node* found = c->find(local_name)) return found;
    }
    return nullptr;
}

void Node::find_all(std::string_view local_name, std::vector<const Node*>& out) const {
    for (const auto& c : children) {
        if (c->is_text()) continue;
        if (c->local() == local_name) out.push_back(c.get());
        c->find_all(local_name, out);
    }
}

std::vector<const Node*> Node::elements(std::string_view local_name) const {
    std::vector<const Node*> out;
    for (const auto& c : children)
        if (!c->is_text() && c->local() == local_name) out.push_back(c.get());
    return out;
}

const Node* Node::child(std::string_view local_name) const {
    for (const auto& c : children)
        if (!c->is_text() && c->local() == local_name) return c.get();
    return nullptr;
}

std::string Node::inner_text() const {
    std::string out;
    collect_text(this, out);
    return collapse_space(out);
}

std::string Node::child_text(std::string_view local_name) const {
    const Node* c = child(local_name);
    return c ? c->inner_text() : std::string();
}

std::string Node::inner_markup() const {
    std::string out;
    for (const auto& c : children) serialise(c.get(), out);
    return out;
}

std::unique_ptr<Node> parse_markup(std::string_view source, MarkupMode mode) {
    return Builder(source, mode).run();
}

}  // namespace lib
