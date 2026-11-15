// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/daisy.h"

#include <stdio.h>

#include <algorithm>
#include <functional>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/epub.h"
#include "doc/html_convert.h"
#include "gfx/image.h"

namespace doc {

namespace {

bool is_heading(std::string_view t) {
    return t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6';
}

std::string file_part(const std::string& target) {
    const size_t hash = target.find('#');
    return hash == std::string::npos ? target : target.substr(0, hash);
}

std::string id_part(const std::string& target) {
    const size_t hash = target.find('#');
    return hash == std::string::npos ? std::string() : target.substr(hash + 1);
}

// DTBook elements as the HTML ones the converter knows. `depth`: the level
// (level1..level6, or nested <level>) the element is in, for its headings.
void dtbook_to_html(lib::Node& n, int depth) {
    for (auto& child : n.children) {
        if (child->is_text()) continue;
        std::string t(child->local());
        int inner = depth;
        if (t.size() == 6 && lib::starts_with(t, "level") && t[5] >= '1' && t[5] <= '6') {
            inner = t[5] - '0';
            t = "section";
        } else if (t == "level") {
            inner = depth + 1;
            t = "section";
        } else if (t == "hd" || t == "levelhd" || t == "bridgehead") {
            t = "h" + std::to_string(std::max(1, std::min(6, depth)));
        } else if (t == "doctitle") {
            t = "h1";
        } else if (t == "docauthor" || t == "covertitle" || t == "byline" || t == "dateline" || t == "author" ||
                   t == "line" || t == "linenum") {
            t = "p";
        } else if (t == "list") {
            t = child->attr("type") == "ol" ? "ol" : "ul";
        } else if (t == "lic" || t == "sent" || t == "w" || t == "abbr" || t == "acronym" || t == "dfn" ||
                   t == "cite" || t == "bdo") {
            t = "span";
        } else if (t == "noteref" || t == "annoref") {
            t = "sup";
        } else if (t == "pagenum") {
            t = "template";  // page numbers of the print book: left out
        } else if (t == "frontmatter" || t == "bodymatter" || t == "rearmatter" || t == "book" || t == "dtbook" ||
                   t == "sidebar" || t == "imggroup" || t == "linegroup" || t == "poem" || t == "notice" ||
                   t == "epigraph" || t == "note" || t == "annotation" || t == "prodnote") {
            t = "div";
        }
        child->tag = t;
        dtbook_to_html(*child, inner);
    }
}

void collect_ids(const lib::Node& n, std::vector<std::string>& out) {
    if (!n.is_text() && !n.attr("id").empty()) out.push_back(n.attr("id"));
    for (const auto& c : n.children) collect_ids(*c, out);
}

std::string first_heading(const lib::Node& n) {
    for (const auto& c : n.children) {
        if (c->is_text()) continue;
        if (is_heading(c->tag)) return lib::collapse_space(c->inner_text());
        const std::string inner = first_heading(*c);
        if (!inner.empty()) return inner;
    }
    return std::string();
}

std::unique_ptr<lib::Node> new_div() {
    auto d = std::make_unique<lib::Node>();
    d->tag = "div";
    return d;
}

// A DAISY 3 package: one whose metadata names the DTBook format.
bool is_dtbook_package(const std::string& opf) {
    return lib::icontains(opf, "Z39.86") || lib::icontains(opf, "x-dtbook") || lib::icontains(opf, "dtbncx");
}

// The package inside a ZIP: an ncc.html or a DAISY 3 .opf.
std::string package_in(Archive& a) {
    for (const auto& e : a.entries())
        if (!e.is_dir && lib::iequals(lib::file_name(e.name), "ncc.html")) return e.name;
    for (const auto& e : a.entries()) {
        if (e.is_dir || !lib::ends_with(lib::lower(e.name), ".opf")) continue;
        std::string text;
        if (a.read(e.name, text, 4ull << 20) && is_dtbook_package(text)) return e.name;
    }
    return std::string();
}

void ncc_metadata(const lib::Node& root, std::string& title, std::string& author) {
    std::vector<const lib::Node*> metas;
    root.find_all("meta", metas);
    for (const lib::Node* m : metas) {
        const std::string name = lib::lower(m->attr("name"));
        if (name == "dc:title" && title.empty()) title = lib::collapse_space(m->attr("content"));
        if (name == "dc:creator" && author.empty()) author = lib::collapse_space(m->attr("content"));
    }
    if (title.empty())
        if (const lib::Node* t = root.find("title")) title = lib::collapse_space(t->inner_text());
}

void opf_metadata(const lib::Node& root, std::string& title, std::string& author) {
    if (const lib::Node* t = root.find("title")) title = lib::collapse_space(t->inner_text());
    if (const lib::Node* c = root.find("creator")) author = lib::collapse_space(c->inner_text());
}

}  // namespace

// ---- the book's files ----------------------------------------------------------------

std::string Daisy::real_name(const std::string& name) const {
    if (archive_ || lib::file_exists(lib::join_path(dir_, name))) return name;
    // Books made on Windows often disagree with themselves about case.
    const size_t slash = name.rfind('/');
    const std::string folder = slash == std::string::npos ? "" : name.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
    auto it = listings_.find(folder);
    if (it == listings_.end()) {
        std::vector<lib::DirEntry> entries;
        lib::list_dir(folder.empty() ? dir_ : lib::join_path(dir_, folder), entries, false);
        std::vector<std::string> names;
        for (const auto& e : entries) names.push_back(e.name);
        it = listings_.emplace(folder, std::move(names)).first;
    }
    for (const std::string& n : it->second)
        if (lib::iequals(n, leaf)) return folder.empty() ? n : folder + "/" + n;
    return name;
}

bool Daisy::read(const std::string& name, std::string& out, uint64_t max) const {
    if (archive_) return archive_->read(name, out, max);
    return lib::read_file(lib::join_path(dir_, real_name(name)), out, max);
}

std::string Daisy::image_key(const std::string& name) const {
    if (archive_) return archive_image_key(path_, name);
    return lib::join_path(dir_, real_name(name));
}

bool Daisy::locate_audio(const std::string& name, const std::string& cache_dir, audio::Track& t) const {
    t.mime = audio_mime_of(name);
    if (t.mime.empty()) return false;
    if (!archive_) {
        t.location = lib::join_path(dir_, real_name(name));
        return lib::file_exists(t.location);
    }
    uint64_t offset = 0, size = 0;
    if (archive_->stored_span(name, offset, size)) {
        t.location = path_;
        t.offset = offset;
        t.slice_size = size;
        return true;
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : path_ + "|" + name) h = (h ^ c) * 1099511628211ull;
    char key[32];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)h);
    const std::string copy = lib::join_path(cache_dir, key + std::string(".") + lib::file_extension(name));
    if (!lib::file_exists(copy)) {
        lib::make_dirs(cache_dir);
        if (!archive_->extract_to(name, copy)) return false;
    }
    t.location = copy;
    return true;
}

// ---- opening -------------------------------------------------------------------------

bool Daisy::open(const std::string& path, std::string& error) {
    path_ = path;
    std::string package;
    if (lib::file_extension(path) == "zip") {
        archive_ = open_archive(path, &error);
        if (!archive_) return false;
        package = package_in(*archive_);
    } else {
        dir_ = lib::parent_dir(path);
        package = lib::file_name(path);
    }
    if (package.empty()) {
        error = "This is not a DAISY book (no ncc.html or DAISY 3 package).";
        return false;
    }
    const bool ok = lib::iequals(lib::file_name(package), "ncc.html") ? open_202(package, error) : open_3(package, error);
    if (!ok) return false;
    if (sections_.empty()) {
        error = "This DAISY book has nothing to show.";
        return false;
    }
    if (title_.empty()) title_ = lib::file_stem(path);
    LOG("daisy: %s: DAISY %s, %zu sections, %zu passages, %zu contents entries, %s", path.c_str(),
        version_ == 2 ? "2.02" : "3", sections_.size(), points_.size(), toc_.size(), has_text_ ? "full text" : "audio and headings");
    return true;
}

void Daisy::parse_smils(const std::vector<std::string>& smils) {
    for (const std::string& smil : smils) {
        std::string xml;
        if (!read(smil, xml, 16ull << 20)) {
            LOG("daisy: %s missing", smil.c_str());
            continue;
        }
        // Sections are not known yet (they come from the text files these name).
        parse_smil(xml, smil, [](const std::string&, const std::string&) { return -1; }, points_);
    }
    for (size_t i = 0; i < points_.size(); ++i) {
        const SyncPoint& p = points_[i];
        if (!p.par_id.empty()) refs_.emplace(p.smil + "#" + p.par_id, (int)i);
        if (!p.text_id.empty()) refs_.emplace(p.smil + "#" + p.text_id, (int)i);
        refs_.emplace(p.smil, (int)i);  // the file alone: its first passage
    }
}

bool Daisy::open_202(const std::string& ncc, std::string& error) {
    version_ = 2;
    std::string text;
    if (!read(ncc, text)) {
        error = "The book's ncc.html could not be read.";
        return false;
    }
    auto root = lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Html);
    ncc_metadata(*root, title_, author_);
    // Headings (h1..h6) link to the SMIL files, in reading order.
    struct Heading { std::string title, target; int level; };
    std::vector<Heading> headings;
    std::vector<std::string> smils;
    std::function<void(const lib::Node&)> walk = [&](const lib::Node& n) {
        for (const auto& c : n.children) {
            if (c->is_text()) continue;
            const lib::Node* a = c->tag == "a" ? c.get() : nullptr;
            if (is_heading(c->tag)) {
                const lib::Node* link = c->find("a");
                if (link && !link->attr("href").empty())
                    headings.push_back({lib::collapse_space(c->inner_text()), archive_resolve(ncc, link->attr("href")),
                                        c->tag[1] - '0'});
            }
            if (a || is_heading(c->tag) || c->tag == "span" || c->tag == "div") {
                std::vector<const lib::Node*> links;
                if (a) links.push_back(a);
                else c->find_all("a", links);
                for (const lib::Node* l : links) {
                    const std::string f = file_part(archive_resolve(ncc, l->attr("href")));
                    if (lib::ends_with(lib::lower(f), ".smil") && std::find(smils.begin(), smils.end(), f) == smils.end())
                        smils.push_back(f);
                }
                if (a || is_heading(c->tag)) continue;
            }
            walk(*c);
        }
    };
    if (const lib::Node* body = root->find("body")) walk(*body);
    parse_smils(smils);

    // The text files the narration points at; a book with no text has only
    // its ncc.html (the headings), which the narration then follows.
    std::vector<std::string> texts;
    for (const SyncPoint& p : points_) {
        const std::string f = file_part(p.text);
        if (!f.empty() && !lib::iequals(lib::file_name(f), "ncc.html") && std::find(texts.begin(), texts.end(), f) == texts.end())
            texts.push_back(f);
    }
    has_text_ = !texts.empty();
    if (!has_text_) texts.push_back(ncc);
    for (const std::string& t : texts) add_text_file(t, false);
    assign_sections();
    for (const Heading& h : headings) add_toc(h.title, h.target, h.level);
    return true;
}

bool Daisy::open_3(const std::string& opf_name, std::string& error) {
    version_ = 3;
    std::string text;
    if (!read(opf_name, text) || !is_dtbook_package(text)) {
        error = "This is not a DAISY 3 book.";
        return false;
    }
    auto opf = lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Xml);
    opf_metadata(*opf, title_, author_);
    struct Item { std::string href, type; };
    std::map<std::string, Item> manifest;
    std::string ncx;
    std::vector<std::string> dtbooks;
    if (const lib::Node* m = opf->find("manifest")) {
        for (const lib::Node* item : m->elements("item")) {
            Item it{archive_resolve(opf_name, item->attr("href")), lib::lower(item->attr("media-type"))};
            manifest[item->attr("id")] = it;
            if (it.type.find("dtbncx") != std::string::npos) ncx = it.href;
            else if (it.type.find("dtbook") != std::string::npos) dtbooks.push_back(it.href);
        }
    }
    std::vector<std::string> smils;
    if (const lib::Node* spine = opf->find("spine")) {
        for (const lib::Node* ref : spine->elements("itemref")) {
            auto found = manifest.find(ref->attr("idref"));
            if (found != manifest.end()) smils.push_back(found->second.href);
        }
    }
    parse_smils(smils);
    std::vector<std::string> texts;
    for (const SyncPoint& p : points_) {
        const std::string f = file_part(p.text);
        if (!f.empty() && std::find(texts.begin(), texts.end(), f) == texts.end()) texts.push_back(f);
    }
    if (texts.empty()) texts = dtbooks;
    for (const std::string& t : texts) add_text_file(t, true);
    has_text_ = !sections_.empty();
    assign_sections();

    // Navigation (NCX): navPoints, nested.
    struct Nav { std::string title, target; int level; };
    std::vector<Nav> navs;
    std::string ncx_text;
    if (!ncx.empty() && read(ncx, ncx_text)) {
        auto root = lib::parse_markup(lib::to_utf8(ncx_text), lib::MarkupMode::Xml);
        std::function<void(const lib::Node&, int)> walk = [&](const lib::Node& n, int level) {
            for (const lib::Node* point : n.elements("navpoint")) {
                const lib::Node* label = point->find("text");
                const lib::Node* content = point->child("content");
                if (label && content)
                    navs.push_back({lib::collapse_space(label->inner_text()), archive_resolve(ncx, content->attr("src")),
                                    level});
                walk(*point, level + 1);
            }
        };
        if (const lib::Node* map = root->find("navmap")) walk(*map, 1);
    }
    if (!has_text_) {
        // Audio with navigation only: a page of the headings, which the
        // narration follows (each passage belongs to the heading before it).
        auto escape = [](const std::string& t) { return lib::replace_all(lib::replace_all(t, "&", "&amp;"), "<", "&lt;"); };
        std::string html = "<div><h1>" + escape(title_) + "</h1>";
        std::vector<std::pair<int, std::string>> starts;  // passage, heading id
        for (size_t i = 0; i < navs.size(); ++i) {
            auto r = refs_.find(navs[i].target);
            if (r == refs_.end()) r = refs_.find(file_part(navs[i].target));
            if (r == refs_.end()) continue;
            const std::string id = "nav" + std::to_string(i);
            const int level = std::max(2, std::min(6, navs[i].level + 1));
            const std::string title = escape(navs[i].title);
            html += "<h" + std::to_string(level) + " id=\"" + id + "\">" + title + "</h" + std::to_string(level) + ">";
            starts.push_back({r->second, id});
        }
        html += "</div>";
        Section s;
        s.file = "#navigation";
        s.title = title_;
        s.root = lib::parse_markup(html, lib::MarkupMode::Html);
        std::sort(starts.begin(), starts.end());
        for (size_t i = 0; i < points_.size(); ++i) {
            points_[i].section = 0;
            points_[i].id.clear();
            for (const auto& [at, id] : starts)
                if (at <= (int)i) points_[i].id = id;
        }
        for (const auto& [at, id] : starts) id_section_["#navigation#" + id] = 0;
        sections_.push_back(std::move(s));
        for (size_t i = 0; i < navs.size(); ++i) toc_.push_back({navs[i].title, 0, "nav" + std::to_string(i), navs[i].level});
        return true;
    }
    for (const Nav& n : navs) add_toc(n.title, n.target, n.level);
    return true;
}

// ---- text ----------------------------------------------------------------------------

void Daisy::add_text_file(const std::string& name, bool dtbook) {
    std::string text;
    if (!read(name, text)) {
        LOG("daisy: text %s missing", name.c_str());
        return;
    }
    const size_t bytes = text.size();
    auto tree = lib::parse_markup(lib::to_utf8(text), dtbook ? lib::MarkupMode::Xml : lib::MarkupMode::Html);
    text = std::string();
    lib::Node* body = const_cast<lib::Node*>(tree->find(dtbook ? "book" : "body"));
    if (!body) body = tree.get();

    // Cut into sections: DTBook at each level1 (front matter before it
    // together); HTML, when large, at each h1.
    std::vector<std::unique_ptr<lib::Node>> parts;
    auto take = [&](std::unique_ptr<lib::Node>& child, bool new_part) {
        if (new_part || parts.empty()) parts.push_back(new_div());
        child->parent = parts.back().get();
        parts.back()->children.push_back(std::move(child));
    };
    if (dtbook) {
        for (auto& matter : body->children) {
            if (matter->is_text()) continue;
            for (auto& child : matter->children) {
                const std::string_view t = child->local();
                take(child, t == "level1" || t == "level");
            }
        }
    } else if (bytes > 150 * 1024) {
        for (auto& child : body->children) take(child, !child->is_text() && child->tag == "h1");
    } else {
        for (auto& child : body->children) take(child, false);
    }
    for (auto& part : parts) {
        if (dtbook) dtbook_to_html(*part, 1);
        Section s;
        s.file = name;
        s.title = first_heading(*part);
        if (s.title.empty()) s.title = title_;
        s.weight = std::max<double>(1, part->inner_text().size());
        const int index = (int)sections_.size();
        std::vector<std::string> ids;
        collect_ids(*part, ids);
        for (const std::string& id : ids) id_section_.emplace(name + "#" + id, index);
        id_section_.emplace(name, index);
        s.root = std::move(part);
        sections_.push_back(std::move(s));
    }
}

int Daisy::section_of(const std::string& file, const std::string& id) const {
    auto it = id.empty() ? id_section_.end() : id_section_.find(file + "#" + id);
    if (it == id_section_.end()) it = id_section_.find(file);
    return it == id_section_.end() ? -1 : it->second;
}

void Daisy::assign_sections() {
    for (SyncPoint& p : points_) {
        if (p.text.empty()) continue;
        p.section = section_of(file_part(p.text), p.id);
    }
    // Narration with no text of its own belongs with what came before it.
    int last = -1;
    for (SyncPoint& p : points_) {
        if (p.section >= 0) last = p.section;
        else p.section = last;
    }
}

void Daisy::add_toc(const std::string& title, const std::string& target, int level) {
    const std::string file = file_part(target);
    int section = -1;
    std::string anchor;
    if (lib::ends_with(lib::lower(file), ".smil")) {
        auto r = refs_.find(target);
        if (r == refs_.end()) r = refs_.find(file);
        if (r == refs_.end()) return;
        section = points_[r->second].section;
        anchor = points_[r->second].id;
    } else {
        section = section_of(file, id_part(target));
        anchor = id_part(target);
    }
    if (section < 0 || title.empty()) return;
    toc_.push_back({title, section, anchor, level});
}

bool Daisy::load_section(int index, Document& out, std::string& error) const {
    if (index < 0 || index >= (int)sections_.size()) {
        error = "No such section.";
        return false;
    }
    const Section& s = sections_[index];
    ConvertOptions options;
    const std::string file = s.file;
    options.resolve = [this, file](const std::string& href) -> std::string {
        if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://") || lib::starts_with(href, "mailto:"))
            return href;
        const std::string target = href[0] == '#' ? file + href : archive_resolve(file, href);
        const std::string plain = file_part(target);
        const std::string ext = lib::file_extension(plain);
        if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" || ext == "bmp")
            return image_key(plain);
        return "daisy:" + target;
    };
    options.image_size = [this](const std::string& key, int& w, int& h) {
        std::string bytes;
        const bool ok = archive_ ? (key.find('\x01') != std::string::npos &&
                                    archive_->read(key.substr(key.find('\x01') + 1), bytes, 32ull << 20))
                                 : lib::read_file(key, bytes, 32ull << 20);
        return ok && gfx::image_dimensions(bytes, w, h);
    };
    options.min_image = 8;
    options.mark_ids = narrated_ids(points_, index);
    html_to_document(*s.root, options, out);
    if (out.title.empty()) out.title = s.title;
    if (out.blocks.empty()) {
        Block b;
        b.runs.push_back({" ", 0, -1});
        out.blocks.push_back(b);
    }
    return true;
}

bool Daisy::resolve(const std::string& href, int& section, std::string& anchor) const {
    if (!lib::starts_with(href, "daisy:")) return false;
    const std::string target = href.substr(6);
    if (lib::ends_with(lib::lower(file_part(target)), ".smil")) {
        // The headings page (ncc.html) links to the narration: where it is read.
        auto r = refs_.find(target);
        if (r == refs_.end()) r = refs_.find(file_part(target));
        if (r == refs_.end()) return false;
        section = points_[r->second].section;
        anchor = points_[r->second].id;
        return section >= 0;
    }
    anchor = id_part(target);
    section = section_of(file_part(target), anchor);
    return section >= 0;
}

bool Daisy::build_narration(const std::string& cache_dir, Timeline& out, std::string& error) const {
    build_timeline(points_, [&](const std::string& name, audio::Track& t) { return locate_audio(name, cache_dir, t); },
                   out);
    if (out.points.empty()) {
        error = "This book's narration could not be played (its audio files are missing or in a format Librarian "
                "cannot play).";
        return false;
    }
    LOG("daisy: narration: %zu passages, %zu audio stretches, %.0f s", out.points.size(), out.tracks.size(), out.duration);
    return true;
}

bool daisy_metadata(const std::string& path, std::string& title, std::string& author) {
    title.clear();
    author.clear();
    std::string text, package = path;
    std::shared_ptr<Archive> archive;
    if (lib::file_extension(path) == "zip") {
        archive = open_archive(path);
        if (!archive) return false;
        package = package_in(*archive);
        if (package.empty() || !archive->read(package, text, 4ull << 20)) return false;
    } else if (!lib::read_file(path, text, 4ull << 20)) {
        return false;
    }
    if (lib::iequals(lib::file_name(package), "ncc.html")) {
        ncc_metadata(*lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Html), title, author);
        return true;
    }
    if (!is_dtbook_package(text)) return false;
    opf_metadata(*lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Xml), title, author);
    return true;
}

}  // namespace doc
