// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/kiwix.h"

#include <stdlib.h>

#include <algorithm>

#include "core/json.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "doc/html_convert.h"
#include "net/http.h"
#include "net/url.h"

namespace svc::kiwix {

namespace {

lib::HttpResponse get(const std::string& url, std::string& error, uint64_t max_bytes = 16ull * 1024 * 1024) {
    lib::HttpRequest request;
    request.url = url;
    request.max_bytes = max_bytes;
    // kiwix-serve can take well over 20 s to answer the first request for a
    // very large ZIM (seen with the full English Wikipedia).
    request.timeout_seconds = 60;
    lib::HttpResponse response = lib::http_request(request);
    if (!response.ok()) error = lib::describe_failure(response);
    return response;
}

}  // namespace

void parse_entries(const std::string& xml, const std::string& base, std::vector<ZimBook>& out, int* total) {
    auto root = lib::parse_markup(xml, lib::MarkupMode::Xml);
    if (total) {
        std::vector<const lib::Node*> found;
        root->find_all("totalresults", found);
        *total = found.empty() ? 0 : atoi(found[0]->inner_text().c_str());
    }
    std::vector<const lib::Node*> entries;
    root->find_all("entry", entries);
    for (const lib::Node* e : entries) {
        ZimBook b;
        b.id = e->child_text("id");
        if (lib::starts_with(b.id, "urn:uuid:")) b.id = b.id.substr(9);
        b.title = e->child_text("title");
        b.summary = e->child_text("summary");
        b.language = e->child_text("language");
        b.name = e->child_text("name");
        b.category = e->child_text("category");
        b.flavour = e->child_text("flavour");
        b.updated = e->child_text("updated").substr(0, 10);
        b.articles = atoll(e->child_text("articlecount").c_str());
        b.media = atoll(e->child_text("mediacount").c_str());
        for (const lib::Node* link : e->elements("link")) {
            const std::string& rel = link->attr("rel");
            const std::string& href = link->attr("href");
            const size_t content = href.find("/content/");
            if (rel.find("image/thumbnail") != std::string::npos) {
                b.icon = lib::resolve_url(base, href);
            } else if (link->attr("type") == "text/html" && content != std::string::npos &&
                       (content == 0 || href.find("://") != std::string::npos)) {
                // "/content/<name>" on a server; the online library's are absolute.
                b.content = href.substr(content + 9);
            } else if (rel.find("acquisition") != std::string::npos && link->attr("type") == "application/x-zim") {
                // A Metalink file listing mirrors; without ".meta4" the address
                // redirects to the nearest mirror itself.
                b.download = lib::resolve_url(base, href);
                if (lib::ends_with(b.download, ".meta4")) b.download.resize(b.download.size() - 6);
                b.size = strtoull(link->attr("length").c_str(), nullptr, 10);
            }
        }
        while (!b.content.empty() && b.content.back() == '/') b.content.pop_back();
        if (!b.content.empty() || !b.download.empty()) out.push_back(std::move(b));
    }
}

void parse_facets(const std::string& xml, bool languages, std::vector<Facet>& out) {
    out.clear();
    auto root = lib::parse_markup(xml, lib::MarkupMode::Xml);
    std::vector<const lib::Node*> entries;
    root->find_all("entry", entries);
    for (const lib::Node* e : entries) {
        Facet f;
        f.title = lib::decode_entities(e->child_text("title"));
        f.code = languages ? e->child_text("language") : f.title;
        f.count = atoi(e->child_text("count").c_str());
        if (!f.code.empty()) out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const Facet& a, const Facet& b) { return lib::natural_less(a.title, b.title); });
}

// The host build has no HTTPS: LIBRARIAN_KIWIX_ONLINE points it at a local copy.
static std::string online_base() {
#ifdef LIB_HOST
    if (const char* e = getenv("LIBRARIAN_KIWIX_ONLINE")) return e;
#endif
    return kOnlineLibrary;
}

bool online_catalog(const OnlineQuery& q, std::vector<ZimBook>& out, int& total, std::string& error, int* status) {
    out.clear();
    total = 0;
    std::string url = online_base() + "/catalog/v2/entries?start=" + std::to_string(q.start) +
                      "&count=" + std::to_string(q.count);
    if (!q.language.empty()) url += "&lang=" + lib::url_encode(q.language);
    if (!q.category.empty()) url += "&category=" + lib::url_encode(q.category);
    if (!q.text.empty()) url += "&q=" + lib::url_encode(q.text);
    lib::HttpResponse r = get(url, error);
    if (status) *status = r.status;
    if (!r.ok()) return false;
    parse_entries(r.body, r.final_url.empty() ? url : r.final_url, out, &total);
    if (total < (int)out.size()) total = q.start + (int)out.size();
    return true;
}

static bool facets(const char* what, bool languages, std::vector<Facet>& out, std::string& error) {
    lib::HttpResponse r = get(online_base() + "/catalog/v2/" + what, error);
    if (!r.ok()) return false;
    parse_facets(r.body, languages, out);
    return !out.empty();
}

bool online_languages(std::vector<Facet>& out, std::string& error) { return facets("languages", true, out, error); }
bool online_categories(std::vector<Facet>& out, std::string& error) { return facets("categories", false, out, error); }

bool catalog(const std::string& server, std::vector<ZimBook>& out, std::string& error, int* status) {
    out.clear();
    lib::HttpResponse r = get(server + "/catalog/v2/entries?count=-1", error);
    if (status) *status = r.status;
    if (!r.ok()) {
        if (r.status == 404) error = "This server has no OPDS v2 catalogue. Librarian needs kiwix-serve 3.2 or newer.";
        return false;
    }
    parse_entries(r.body, server + "/", out);
    // A server's catalogue lists what it serves.
    out.erase(std::remove_if(out.begin(), out.end(), [](const ZimBook& b) { return b.content.empty(); }), out.end());
    std::sort(out.begin(), out.end(), [](const ZimBook& a, const ZimBook& b) { return lib::natural_less(a.title, b.title); });
    LOG("kiwix: %zu books", out.size());
    if (out.empty()) error = "The server's catalogue lists no ZIM files.";
    return !out.empty();
}

}  // namespace svc::kiwix

namespace svc {

std::string ZimBook::file_name() const {
    std::string from = download;
    const size_t q = from.find_first_of("?#");
    if (q != std::string::npos) from.resize(q);
    std::string n = lib::file_name(from);
    if (n.empty()) n = (content.empty() ? name : content) + ".zim";
    return lib::safe_file_name(n);
}

}  // namespace svc

namespace svc::kiwix {

std::string main_page_url(const std::string& server, const ZimBook& book) { return server + "/content/" + book.content; }

std::string random_url(const std::string& server, const std::string& content) {
    return server + "/random?content=" + lib::url_encode(content);
}

bool suggest(const std::string& server, const std::string& content, const std::string& term, int count,
             std::vector<SearchHit>& out, std::string& error) {
    out.clear();
    lib::HttpResponse r = get(server + "/suggest?content=" + lib::url_encode(content) + "&term=" + lib::url_encode(term) +
                                  "&count=" + std::to_string(count),
                              error);
    if (!r.ok()) return false;
    bool ok = false;
    const lib::Json j = lib::Json::parse(r.body, &ok);
    if (!ok || !j.is_array()) {
        error = "The server's suggestions could not be read.";
        return false;
    }
    for (const lib::Json& item : j.items()) {
        // "pattern" entries are the "containing ..." full-text prompt.
        if (item["kind"].str() != "path" || item["path"].str().empty()) continue;
        SearchHit hit;
        hit.title = lib::decode_entities(item["value"].str());
        hit.url = server + "/content/" + content + "/" + lib::url_encode(item["path"].str(), true);
        out.push_back(std::move(hit));
    }
    return true;
}

bool search(const std::string& server, const std::string& content, const std::string& pattern, int start, int count,
            std::vector<SearchHit>& out, int& total, std::string& error) {
    out.clear();
    total = 0;
    lib::HttpResponse r = get(server + "/search?books.name=" + lib::url_encode(content) + "&pattern=" +
                                  lib::url_encode(pattern) + "&format=xml&start=" + std::to_string(start + 1) +
                                  "&pageLength=" + std::to_string(count),
                              error);
    if (!r.ok()) {
        if (r.status == 404 || r.status == 400)
            error = "Full-text search is not available for this collection (it may have no search index).";
        return false;
    }
    auto root = lib::parse_markup(r.body, lib::MarkupMode::Xml);
    if (const lib::Node* t = root->find("totalresults")) total = atoi(t->inner_text().c_str());
    std::vector<const lib::Node*> items;
    root->find_all("item", items);
    for (const lib::Node* item : items) {
        SearchHit hit;
        hit.title = item->child_text("title");
        hit.url = lib::resolve_url(server + "/", item->child_text("link"));
        // The description holds <b> highlights: markup inside RSS text.
        if (const lib::Node* d = item->child("description")) hit.snippet = lib::strip_tags(d->inner_markup());
        hit.snippet = lib::decode_entities(hit.snippet);
        hit.words = item->child_text("wordcount");
        out.push_back(std::move(hit));
    }
    return true;
}

bool split_article_url(const std::string& url, std::string& content, std::string& path) {
    lib::Url u;
    if (!lib::parse_url(url, u)) return false;
    const std::string p = lib::url_path_only(u.path);
    if (!lib::starts_with(p, "/content/")) return false;
    const size_t slash = p.find('/', 9);
    if (slash == std::string::npos) return false;
    content = p.substr(9, slash - 9);
    path = lib::url_decode(p.substr(slash + 1));
    return !content.empty() && !path.empty();
}

bool is_article_link(const std::string& server, const std::string& url) {
    std::string content, path;
    return lib::same_origin(server, url) && split_article_url(url, content, path);
}

bool fetch_article(const std::string& url, Article& out, std::string& error) {
    lib::HttpResponse r = get(url, error, 24ull * 1024 * 1024);
    if (!r.ok()) return false;
    if (!r.content_type.empty() && !lib::icontains(r.content_type, "html")) {
        error = "This entry is not an article (" + r.content_type + ").";
        return false;
    }
    out.url = r.final_url;
    std::string content;
    split_article_url(out.url, content, out.path);
    doc::ConvertOptions options;
    const std::string base = out.url;
    options.resolve = [base](const std::string& href) { return lib::resolve_url(base, href); };
    out.doc = doc::html_to_document(lib::to_utf8(r.body), options);
    // The page's own top heading is the best title (some ZIMs title every page "index").
    if (!out.doc.blocks.empty() && out.doc.blocks[0].kind == doc::BlockKind::Heading && out.doc.blocks[0].level == 1) {
        std::string heading;
        for (const auto& r : out.doc.blocks[0].runs) heading += r.text;
        heading = lib::collapse_space(heading);
        if (!heading.empty()) out.doc.title = heading;
    }
    out.title = out.doc.title.empty() ? out.path : out.doc.title;
    if (looks_like_script_app(r.body, out.doc)) {
        error = "This collection is an interactive web app: its pages are built by JavaScript, which Librarian "
                "cannot run. Text collections such as Wikipedia, Wikibooks or DevDocs work normally.";
        return false;
    }
    if (out.doc.blocks.empty()) {
        error = "The article is empty.";
        return false;
    }
    return true;
}

bool looks_like_script_app(const std::string& html, const doc::Document& converted) {
    // Almost no text of its own, and the marks of a page filled in by scripts:
    // many scripts, a <noscript> warning, or an empty mount point.
    if (converted.text_size() >= 300) return false;
    size_t scripts = 0;
    for (size_t at = 0; (at = html.find("<script", at)) != std::string::npos; at += 7) ++scripts;
    const bool mount = html.find("id=\"app\"") != std::string::npos || html.find("id=\"root\"") != std::string::npos ||
                       html.find("id=\"loader\"") != std::string::npos;
    return scripts >= 3 || mount || html.find("<noscript") != std::string::npos;
}

}  // namespace svc::kiwix
