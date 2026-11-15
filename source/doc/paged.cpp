// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "doc/paged.h"

#include <string.h>

#include <algorithm>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/archive.h"

namespace doc {

namespace {

bool is_image_name(const std::string& name) {
    const std::string ext = lib::file_extension(name);
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" || ext == "bmp";
}

// A comic archive: its pictures, in natural order ("page 9" before "page 10").
class ComicDocument : public PagedDocument {
public:
    bool open(const std::string& path, std::string& error) {
        archive_ = open_archive(path, &error);
        if (!archive_) return false;
        for (const auto& e : archive_->entries()) {
            if (e.is_dir || !is_image_name(e.name)) continue;
            // macOS resource forks and thumbnails are not pages.
            if (e.name.find("__MACOSX") != std::string::npos || lib::file_name(e.name)[0] == '.') continue;
            pages_.push_back(e.name);
        }
        std::sort(pages_.begin(), pages_.end(), [](const std::string& a, const std::string& b) { return lib::natural_less(a, b); });
        if (pages_.empty()) {
            error = "The archive contains no pictures.";
            return false;
        }
        return true;
    }

    int page_count() const override { return (int)pages_.size(); }

    bool render(int index, int max_w, int max_h, gfx::Image& out, std::string& error) override {
        if (index < 0 || index >= (int)pages_.size()) return false;
        std::string bytes;
        if (!archive_->read(pages_[index], bytes, 128ull * 1024 * 1024)) {
            error = "Page " + std::to_string(index + 1) + " could not be read.";
            return false;
        }
        // Decoded straight to the requested size bound: a 4000-pixel scan never
        // stays in memory at full size.
        if (!gfx::decode_image(bytes, out, max_w, max_h)) {
            error = "Page " + std::to_string(index + 1) + " is not a picture Librarian can decode.";
            return false;
        }
        if (out.w < max_w && out.h < max_h) out = gfx::fit_image(out, max_w, max_h);
        return true;
    }

    bool first_page_bytes(std::string& bytes) { return !pages_.empty() && archive_->read(pages_[0], bytes, 64ull * 1024 * 1024); }

private:
    std::shared_ptr<Archive> archive_;
    std::vector<std::string> pages_;
};

}  // namespace

std::shared_ptr<PagedDocument> open_paged(const std::string& path, std::string& error) {
    const std::string ext = lib::file_extension(path);
    if (ext == "pdf") return open_pdf(path, error);
    auto comic = std::make_shared<ComicDocument>();
    if (!comic->open(path, error)) return nullptr;
    return comic;
}

bool paged_cover(const std::string& path, std::string& bytes) {
    if (lib::file_extension(path) == "pdf") {
        std::string error;
        // One small page: no need for the reader's large cache. A page's pictures
        // may still be decoded at full size: one such render at a time.
        gfx::BigDecodeLock big;
        auto pdf = open_pdf(path, error, 8);
        gfx::Image page;
        if (!pdf || !pdf->render(0, 400, 600, page, error)) return false;
        bytes = encode_bmp(page);
        return true;
    }
    ComicDocument comic;
    std::string error;
    return comic.open(path, error) && comic.first_page_bytes(bytes);
}

std::string encode_bmp(const gfx::Image& img) {
    // BITMAPFILEHEADER + BITMAPINFOHEADER, 32 bits per pixel, bottom-up rows.
    const uint32_t data = (uint32_t)img.w * img.h * 4, file = 54 + data;
    std::string out(54, '\0');
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&out[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&out[at], &v, 2); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, file);
    put32(10, 54);
    put32(14, 40);
    put32(18, (uint32_t)img.w);
    put32(22, (uint32_t)img.h);
    put16(26, 1);
    put16(28, 32);
    put32(34, data);
    out.reserve(file);
    for (int y = img.h - 1; y >= 0; --y)
        out.append((const char*)(img.px.data() + (size_t)y * img.w), (size_t)img.w * 4);
    return out;
}

}  // namespace doc
