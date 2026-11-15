// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/opds.h"

#include <stdlib.h>

#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "net/http.h"
#include "net/url.h"

namespace svc {

std::string format_for_mime(const std::string& type, const std::string& href) {
    const std::string t = lib::lower(type);
    if (t.find("epub") != std::string::npos) return "epub";
    if (t.find("pdf") != std::string::npos) return "pdf";
    if (t.find("cbz") != std::string::npos || t.find("comicbook+zip") != std::string::npos) return "cbz";
    if (t.find("cbr") != std::string::npos || t.find("comicbook-rar") != std::string::npos || t.find("rar") != std::string::npos) return "cbr";
    if (t.find("azw3") != std::string::npos || t.find("mobi8") != std::string::npos) return "azw3";
    if (t.find("djvu") != std::string::npos) return "djvu";
    if (t.find("mobi") != std::string::npos || t.find("kindle") != std::string::npos) return "mobi";
    if (t.find("fb2") != std::string::npos || t.find("fictionbook") != std::string::npos) return "fb2";
    if (t == "text/plain") return "txt";
    if (t.find("html") != std::string::npos) return "html";
    // Calibre-Web's links end in the format: /opds/download/12/epub/
    std::string path = lib::lower(lib::url_path_only(href));
    while (!path.empty() && path.back() == '/') path.pop_back();
    const size_t slash = path.rfind('/');
    std::string last = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = last.rfind('.');
    if (dot != std::string::npos) last = last.substr(dot + 1);
    return last.size() <= 5 ? last : std::string();
}

bool opds_parse(const std::string& xml, const std::string& base, OpdsFeed& out) {
    out = OpdsFeed();
    auto root = lib::parse_markup(xml, lib::MarkupMode::Xml);
    const lib::Node* feed = root->find("feed");
    if (!feed) return false;
    out.title = feed->child_text("title");
    for (const lib::Node* link : feed->elements("link")) {
        const std::string& rel = link->attr("rel");
        const std::string href = lib::resolve_url(base, link->attr("href"));
        if (rel == "next") out.next = href;
        else if (rel == "previous" || rel == "prev") out.previous = href;
        else if (rel == "search" && out.search.empty()) out.search = href;
    }
    for (const lib::Node* e : feed->elements("entry")) {
        OpdsEntry entry;
        entry.id = e->child_text("id");
        entry.title = e->child_text("title");
        std::vector<const lib::Node*> authors;
        for (const lib::Node* a : e->elements("author")) {
            const std::string name = a->child_text("name");
            if (name.empty()) continue;
            if (!entry.author.empty()) entry.author += ", ";
            entry.author += name;
        }
        entry.updated = e->child_text("updated");
        entry.language = e->child_text("language");
        entry.publisher = e->child_text("publisher");
        entry.issued = e->child_text("issued");
        for (const lib::Node* c : e->elements("category")) {
            const std::string label = c->attr("label").empty() ? c->attr("term") : c->attr("label");
            if (!label.empty()) entry.categories.push_back(label);
        }
        // Summary: <content type="xhtml|html"> or <summary>.
        if (const lib::Node* content = e->child("content")) {
            const std::string type = content->attr("type");
            entry.summary = type == "xhtml" ? lib::strip_tags(content->inner_markup())
                            : type == "html" ? lib::strip_tags(content->inner_text())
                                             : content->inner_text();
        }
        if (entry.summary.empty()) entry.summary = lib::strip_tags(e->child_text("summary"));

        for (const lib::Node* link : e->elements("link")) {
            const std::string& rel = link->attr("rel");
            const std::string& type = link->attr("type");
            const std::string href = lib::resolve_url(base, link->attr("href"));
            if (link->attr("href").empty()) continue;
            if (rel.find("opds-spec.org/acquisition") != std::string::npos) {
                OpdsAcquisition acq{href, type, format_for_mime(type, href), (uint64_t)atoll(link->attr("length").c_str())};
                if (!acq.format.empty()) entry.formats.push_back(acq);
            } else if (rel == "http://opds-spec.org/image" || rel == "http://opds-spec.org/cover" ||
                       rel == "x-stanza-cover-image") {
                entry.cover = href;
            } else if (rel.find("image/thumbnail") != std::string::npos || rel == "x-stanza-cover-image-thumbnail") {
                entry.thumbnail = href;
            } else if (type.find("opds-catalog") != std::string::npos || type.find("atom+xml") != std::string::npos ||
                       rel == "subsection" || rel.find("opds-spec.org/sort") != std::string::npos) {
                if (entry.link.empty()) entry.link = href;
            }
        }
        if (entry.cover.empty()) entry.cover = entry.thumbnail;
        if (entry.thumbnail.empty()) entry.thumbnail = entry.cover;
        entry.navigation = entry.formats.empty() && !entry.link.empty();
        // Calibre-Web: /opds/download/<id>/<format>/ or /opds/cover/<id>.
        for (const auto& f : entry.formats) {
            const size_t at = f.href.find("/download/");
            if (at != std::string::npos) entry.book_id = f.href.substr(at + 10, f.href.find('/', at + 10) - at - 10);
        }
        // Series: Calibre-Web puts it in the content ("Series: Name [2]") or
        // a calibre:series element in some versions.
        const std::string series = e->child_text("series");
        if (!series.empty()) {
            entry.series = series;
            entry.series_index = atof(e->child_text("series_index").c_str());
        }
        if (!entry.title.empty()) out.entries.push_back(std::move(entry));
    }
    return true;
}

bool opds_fetch(const std::string& url, OpdsFeed& out, std::string& error, int* status) {
    lib::HttpRequest request;
    request.url = url;
    request.headers.emplace_back("Accept", "application/atom+xml, application/xml;q=0.9, */*;q=0.5");
    lib::HttpResponse r = lib::http_request(request);
    if (status) *status = r.status;
    if (!r.ok()) {
        error = lib::describe_failure(r);
        return false;
    }
    if (!opds_parse(lib::to_utf8(r.body), r.final_url, out)) {
        error = "The server did not send an OPDS catalogue. Is OPDS enabled in Calibre-Web?";
        return false;
    }
    return true;
}

bool opds_search_template(const std::string& link, std::string& out) {
    out.clear();
    if (link.empty()) return false;
    if (link.find("{searchTerms}") != std::string::npos) {
        out = link;
        return true;
    }
    // An OpenSearch description: its Url element for Atom results (any, else).
    lib::HttpRequest request;
    request.url = link;
    request.timeout_seconds = 15;
    request.max_bytes = 256 * 1024;
    const lib::HttpResponse r = lib::http_request(request);
    return r.ok() && opds_parse_opensearch(r.body, r.final_url.empty() ? link : r.final_url, out);
}

bool opds_parse_opensearch(const std::string& xml, const std::string& base, std::string& out) {
    out.clear();
    // (The markup parser gives element names in lower case.)
    auto root = lib::parse_markup(xml, lib::MarkupMode::Xml);
    const lib::Node* description = root->find("opensearchdescription");
    if (!description) return false;
    std::string any;
    for (const lib::Node* u : description->elements("url")) {
        const std::string t = u->attr("template");
        if (t.find("{searchTerms}") == std::string::npos) continue;
        const std::string resolved = lib::resolve_url(base, t);
        if (lib::starts_with(u->attr("type"), "application/atom+xml")) {
            out = resolved;
            return true;
        }
        if (any.empty()) any = resolved;
    }
    out = any;
    return !out.empty();
}

std::string opds_search_url(const std::string& tmpl, const std::string& query) {
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const size_t end = tmpl.find('}', i);
            if (end != std::string::npos) {
                const std::string name = tmpl.substr(i + 1, end - i - 1);
                if (name == "searchTerms") out += lib::url_encode(query);
                // Optional parameters ("{startPage?}") are left empty.
                i = end + 1;
                continue;
            }
        }
        out += tmpl[i++];
    }
    return out;
}

}  // namespace svc
