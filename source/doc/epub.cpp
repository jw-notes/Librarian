// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/epub.h"

#include <algorithm>
#include <functional>
#include <map>
#include <stdio.h>

#include "core/fs.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "doc/html_convert.h"
#include "gfx/image.h"
#include "gfx/image_cache.h"

namespace doc {

namespace {

constexpr char kSep = '\x01';

std::string strip_fragment(const std::string& s) {
    const size_t hash = s.find('#');
    return hash == std::string::npos ? s : s.substr(0, hash);
}

}  // namespace

std::string archive_image_key(const std::string& file, const std::string& inner) {
    return "archive:" + file + kSep + inner;
}

void register_archive_images() {
    gfx::image_register_prefix_loader("archive:", [](const std::string& source, std::string& bytes) {
        const size_t sep = source.find(kSep);
        if (sep == std::string::npos) return false;
        auto archive = open_archive(source.substr(8, sep - 8));
        return archive && archive->read(source.substr(sep + 1), bytes, 64ull * 1024 * 1024);
    });
}

std::string Epub::image_key(const std::string& path) const { return archive_image_key(file_, path); }

int Epub::chapter_of(const std::string& path) const {
    const std::string p = strip_fragment(path);
    for (size_t i = 0; i < chapters_.size(); ++i)
        if (chapters_[i].path == p || lib::iequals(chapters_[i].path, p)) return (int)i;
    return -1;
}

bool Epub::open(const std::string& file, std::string& error) {
    file_ = file;
    archive_ = open_archive(file, &error);
    if (!archive_) return false;

    // META-INF/container.xml names the package document.
    std::string text, opf_path;
    if (archive_->read("META-INF/container.xml", text)) {
        auto root = lib::parse_markup(text, lib::MarkupMode::Xml);
        if (const lib::Node* rf = root->find("rootfile")) opf_path = rf->attr("full-path");
    }
    if (opf_path.empty()) {
        // Some broken books have no container: take the first .opf.
        for (const auto& e : archive_->entries())
            if (lib::ends_with(lib::lower(e.name), ".opf")) {
                opf_path = e.name;
                break;
            }
    }
    if (opf_path.empty() || !archive_->read(opf_path, text)) {
        error = "This EPUB has no package document (content.opf).";
        return false;
    }
    auto opf = lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Xml);

    if (const lib::Node* meta = opf->find("metadata")) {
        title_ = meta->child_text("title");
        std::vector<const lib::Node*> creators;
        meta->find_all("creator", creators);
        for (const lib::Node* c : creators) {
            if (!author_.empty()) author_ += ", ";
            author_ += c->inner_text();
        }
        language_ = meta->child_text("language");
        description_ = lib::strip_tags(meta->child_text("description"));
    }

    struct Item { std::string href, type, properties, overlay; };
    std::map<std::string, Item> manifest;
    std::string cover_id, nav_path, ncx_path;
    if (const lib::Node* m = opf->find("manifest")) {
        for (const lib::Node* item : m->elements("item")) {
            Item it{archive_resolve(opf_path, item->attr("href")), item->attr("media-type"), item->attr("properties"),
                    item->attr("media-overlay")};
            manifest[item->attr("id")] = it;
            if (it.properties.find("nav") != std::string::npos) nav_path = it.href;
            if (it.properties.find("cover-image") != std::string::npos) cover_ = it.href;
        }
    }
    // EPUB 2 cover: <meta name="cover" content="item-id"/>.
    if (cover_.empty()) {
        std::vector<const lib::Node*> metas;
        opf->find_all("meta", metas);
        for (const lib::Node* m : metas)
            if (m->attr("name") == "cover") cover_id = m->attr("content");
        auto found = manifest.find(cover_id);
        if (found != manifest.end() && lib::starts_with(found->second.type, "image/")) cover_ = found->second.href;
    }
    if (cover_.empty()) {
        for (const auto& [id, it] : manifest)
            if (lib::starts_with(it.type, "image/") && lib::icontains(id + it.href, "cover")) {
                cover_ = it.href;
                break;
            }
    }

    if (const lib::Node* spine = opf->find("spine")) {
        auto ncx = manifest.find(spine->attr("toc"));
        if (ncx != manifest.end()) ncx_path = ncx->second.href;
        for (const lib::Node* ref : spine->elements("itemref")) {
            auto found = manifest.find(ref->attr("idref"));
            if (found == manifest.end()) continue;
            const Item& it = found->second;
            if (!lib::icontains(it.type, "html") && !lib::icontains(it.type, "xml")) continue;
            std::string overlay;
            if (!it.overlay.empty()) {
                auto smil = manifest.find(it.overlay);
                if (smil != manifest.end()) overlay = smil->second.href;
            }
            chapters_.push_back({it.href, archive_->size_of(it.href), overlay});
        }
    }
    if (chapters_.empty()) {
        error = "This EPUB lists no chapters.";
        return false;
    }
    // No cover named anywhere (common in picture books and read-along books):
    // the first picture of the first chapter, which is usually the cover.
    if (cover_.empty()) {
        std::string first;
        if (archive_->read(chapters_[0].path, first, 2ull << 20)) {
            auto doc = lib::parse_markup(lib::to_utf8(first), lib::MarkupMode::Html);
            const lib::Node* img = doc->find("img");
            std::string src = img ? img->attr("src") : std::string();
            if (src.empty())
                if (const lib::Node* image = doc->find("image")) src = image->attr("xlink:href").empty() ? image->attr("href") : image->attr("xlink:href");
            if (!src.empty()) {
                const std::string path = archive_resolve(chapters_[0].path, src);
                const std::string ext = lib::lower(path.substr(path.rfind('.') + 1));
                if ((ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp") && archive_->has(path))
                    cover_ = path;
            }
        }
    }
    if (!nav_path.empty()) parse_nav(nav_path);
    if (toc_.empty() && !ncx_path.empty()) parse_ncx(ncx_path);
    if (title_.empty()) title_ = file_;
    LOG("epub: %zu chapters, %zu contents entries, cover %s", chapters_.size(), toc_.size(), cover_.empty() ? "no" : "yes");
    return true;
}

void Epub::parse_ncx(const std::string& path) {
    std::string text;
    if (!archive_->read(path, text)) return;
    auto root = lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Xml);
    const lib::Node* map = root->find("navmap");
    if (!map) return;
    std::function<void(const lib::Node&, int)> walk = [&](const lib::Node& n, int level) {
        for (const lib::Node* point : n.elements("navpoint")) {
            const lib::Node* label = point->find("text");
            const lib::Node* content = point->child("content");
            if (label && content) {
                const std::string target = archive_resolve(path, content->attr("src"));
                const size_t hash = target.find('#');
                toc_.push_back({label->inner_text(), strip_fragment(target), hash == std::string::npos ? "" : target.substr(hash + 1), level});
            }
            walk(*point, level + 1);
        }
    };
    walk(*map, 1);
}

void Epub::parse_nav(const std::string& path) {
    std::string text;
    if (!archive_->read(path, text)) return;
    auto root = lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Html);
    std::vector<const lib::Node*> navs;
    root->find_all("nav", navs);
    const lib::Node* toc = nullptr;
    for (const lib::Node* n : navs)
        if (n->attr("type") == "toc" || n->attr("epub:type") == "toc") toc = n;
    if (!toc && !navs.empty()) toc = navs[0];
    if (!toc) return;
    std::function<void(const lib::Node&, int)> walk = [&](const lib::Node& list, int level) {
        for (const lib::Node* li : list.elements("li")) {
            const lib::Node* a = li->child("a");
            if (!a) a = li->find("a");
            if (a && !a->attr("href").empty()) {
                const std::string target = archive_resolve(path, a->attr("href"));
                const size_t hash = target.find('#');
                toc_.push_back({a->inner_text(), strip_fragment(target), hash == std::string::npos ? "" : target.substr(hash + 1), level});
            }
            if (const lib::Node* sub = li->child("ol")) walk(*sub, level + 1);
        }
    };
    if (const lib::Node* ol = toc->find("ol")) walk(*ol, 1);
}

bool Epub::load_chapter(int index, Document& out, std::string& error) const {
    if (index < 0 || index >= (int)chapters_.size()) {
        error = "No such chapter.";
        return false;
    }
    const std::string path = chapters_[index].path;
    std::string text;
    if (!archive_->read(path, text, 32ull * 1024 * 1024)) {
        error = "The chapter " + path + " could not be read from the book.";
        return false;
    }
    ConvertOptions options;
    const std::string file = file_;
    std::shared_ptr<Archive> archive = archive_;
    options.resolve = [path, file, archive](const std::string& href) -> std::string {
        if (lib::starts_with(href, "http://") || lib::starts_with(href, "https://") || lib::starts_with(href, "mailto:"))
            return href;
        const std::string target = archive_resolve(path, href);
        const std::string plain = strip_fragment(target);
        // Pictures become image-cache keys; anything else is an internal link.
        const std::string ext = lib::lower(plain.substr(plain.rfind('.') + 1));
        if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" || ext == "bmp" || ext == "svg")
            return archive_image_key(file, plain);
        return "epub:" + target;
    };
    options.image_size = [archive](const std::string& key, int& w, int& h) {
        const size_t sep = key.find(kSep);
        std::string bytes;
        if (sep == std::string::npos || !archive->read(key.substr(sep + 1), bytes, 32ull * 1024 * 1024)) return false;
        return gfx::image_dimensions(bytes, w, h);
    };
    options.min_image = 8;
    // Narrated passages become marks, so the reader can follow the narration.
    if (!chapters_[index].overlay.empty()) {
        std::vector<SyncPoint> points;
        overlay_points(index, points);
        options.mark_ids = narrated_ids(points, index);
    }
    html_to_document(*lib::parse_markup(lib::to_utf8(text), lib::MarkupMode::Html), options, out);
    if (out.blocks.empty()) {
        // An image-only page (a cover or plate) wrapped in SVG: <image xlink:href>.
        Block b;
        b.kind = BlockKind::Paragraph;
        b.runs.push_back({" ", 0, -1});
        out.blocks.push_back(b);
    }
    return true;
}

void Epub::overlay_points(int chapter, std::vector<SyncPoint>& out) const {
    const std::string& smil = chapters_[chapter].overlay;
    std::string xml;
    if (smil.empty() || !archive_->read(smil, xml, 16ull * 1024 * 1024)) return;
    parse_smil(xml, smil, [this](const std::string& path, const std::string&) { return chapter_of(path); }, out);
}

bool Epub::has_narration() const {
    for (const auto& c : chapters_)
        if (!c.overlay.empty()) return true;
    return false;
}

bool Epub::build_narration(const std::string& cache_dir, Timeline& out, std::string& error) const {
    std::vector<SyncPoint> points;
    // Each overlay once, in reading order (several chapters may share one).
    std::vector<std::string> seen;
    for (int i = 0; i < (int)chapters_.size(); ++i) {
        const std::string& smil = chapters_[i].overlay;
        if (smil.empty() || std::find(seen.begin(), seen.end(), smil) != seen.end()) continue;
        seen.push_back(smil);
        overlay_points(i, points);
    }
    const std::string file = file_;
    std::shared_ptr<Archive> archive = archive_;
    build_timeline(std::move(points),
                   [&](const std::string& name, audio::Track& t) {
                       t.mime = audio_mime_of(name);
                       if (t.mime.empty()) return false;
                       uint64_t offset = 0, size = 0;
                       if (archive->stored_span(name, offset, size)) {
                           t.location = file;
                           t.offset = offset;
                           t.slice_size = size;
                           return true;
                       }
                       // Compressed: a copy on the console, kept while the book is read.
                       uint64_t h = 1469598103934665603ull;
                       for (unsigned char c : file + "|" + name) h = (h ^ c) * 1099511628211ull;
                       char key[32];
                       snprintf(key, sizeof key, "%016llx", (unsigned long long)h);
                       const std::string copy = lib::join_path(cache_dir, key + std::string(".") +
                                                               lib::file_extension(name));
                       if (!lib::file_exists(copy)) {
                           lib::make_dirs(cache_dir);
                           if (!archive->extract_to(name, copy)) return false;
                       }
                       t.location = copy;
                       return true;
                   },
                   out);
    if (out.points.empty()) {
        error = "This book's narration could not be read.";
        return false;
    }
    LOG("epub: narration: %zu passages, %zu audio stretches, %.0f s", out.points.size(), out.tracks.size(), out.duration);
    return true;
}

}  // namespace doc
