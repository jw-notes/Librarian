// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Decoded images: JPEG, PNG, GIF (first frame), BMP and WebP.
#include <stdint.h>

#include <string>
#include <vector>

namespace gfx {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // 0xAARRGGBB, straight alpha
    bool opaque = true;
    // Pixels per logical pixel (see gfx/canvas.h): images loaded for the
    // screen are decoded at its resolution, so their logical size is lw x lh.
    float density = 1.f;
    int lw() const { return density == 1.f ? w : (int)(w / density + 0.5f); }
    int lh() const { return density == 1.f ? h : (int)(h / density + 0.5f); }

    bool empty() const { return px.empty(); }
    size_t bytes() const { return px.size() * 4; }
};

// Decodes `data`. Images larger than max_w x max_h are scaled down to fit
// (keeping the aspect ratio), so a huge photo never stays in memory at full size.
// Big pictures are decoded one at a time, and pictures over 16 megapixels are
// refused: on the console, decoding one would need most of the free memory.
bool decode_image(const std::string& data, Image& out, int max_w = 4096, int max_h = 4096);

// Held around other work that needs a full-size picture in memory for a while
// (rendering a PDF page for a cover), so it never coincides with another.
class BigDecodeLock {
public:
    BigDecodeLock();
    ~BigDecodeLock();
    BigDecodeLock(const BigDecodeLock&) = delete;
    BigDecodeLock& operator=(const BigDecodeLock&) = delete;
};
// Reads only the dimensions; false if the format is not recognised.
bool image_dimensions(const std::string& data, int& w, int& h);
// Area-averaged when shrinking, bilinear when enlarging.
Image scale_image(const Image& src, int w, int h);
// Scales to cover w x h (cropping the overflow from the centre).
Image cover_image(const Image& src, int w, int h);
// Scales to fit inside w x h, keeping the aspect ratio.
Image fit_image(const Image& src, int w, int h);
// JPEG at `quality` (1-100); transparency is flattened onto white.
bool encode_jpeg(const Image& image, int quality, std::string& out);

}  // namespace gfx
