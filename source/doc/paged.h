// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Fixed-layout documents shown a page at a time: comics (CBZ, CBR) and PDF.
// Pages are rendered on worker threads at the size they will be shown, so
// memory stays proportional to the screen, not to the file.
#include <memory>
#include <string>
#include <vector>

#include "gfx/image.h"
#include "gfx/canvas.h"

namespace doc {

struct OutlineEntry {
    std::string title;
    int page = 0;
    int level = 1;
};

struct PageInfo { float width = 0, height = 0; };
struct PageLink {
    float x = 0, y = 0, w = 0, h = 0;  // PDF points, relative to page bounds
    int page = -1;                    // internal destination, -1 for external
    float dest_x = 0, dest_y = 0;
    std::string uri;
};

class PagedDocument {
public:
    virtual ~PagedDocument() = default;
    virtual int page_count() const = 0;
    // Renders page `index` to fit inside max_w x max_h. Safe to call from
    // several threads (implementations serialise internally).
    virtual bool render(int index, int max_w, int max_h, gfx::Image& out, std::string& error) = 0;
    virtual PageInfo page_info(int index) { return {}; }
    virtual std::vector<PageLink> links(int index) { return {}; }
    // A bounded region in scaled, clockwise-rotated page coordinates.
    virtual bool render_region(int index, float scale, int rotation, const gfx::Rect& region,
                               gfx::Image& out, std::string& error) { return false; }
    virtual std::vector<OutlineEntry> outline() { return {}; }
    virtual std::string title() const { return std::string(); }
    virtual std::string author() const { return std::string(); }
    virtual bool is_pdf() const { return false; }
};

std::shared_ptr<PagedDocument> open_paged(const std::string& path, std::string& error);
// The first page as image bytes (for covers in the Local library).
bool paged_cover(const std::string& path, std::string& bytes);
// Encodes pixels as an uncompressed 32-bit BMP (cheap, and decodable by the
// image cache) for covers that are rendered rather than stored.
std::string encode_bmp(const gfx::Image& img);

// Implemented in pdf.cpp (MuPDF) when built with it. `store_mb`: MuPDF's cache
// of decoded resources; a reader wants it large, a cover or metadata small.
std::shared_ptr<PagedDocument> open_pdf(const std::string& path, std::string& error, int store_mb = 64);

}  // namespace doc
