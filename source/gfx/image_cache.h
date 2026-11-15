// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Asynchronous, size-aware image cache for covers, collection icons and
// article pictures. A request names a source (http(s) URL or a local file)
// and the box it will be drawn in; the cache downloads and decodes on a
// worker, keeps a scaled copy per size, and evicts least-recently-used images
// to stay inside a memory budget.
#include <functional>
#include <string>

#include "gfx/image.h"

namespace gfx {

enum class Fit { Contain, Cover };
enum class ImageState { Loading, Ready, Failed };

// Returns the image scaled for a w x h box, or nullptr while it is loading or
// if it failed (see image_state). Must be called on the UI thread.
const Image* image_get(const std::string& source, int w, int h, Fit fit = Fit::Contain);
ImageState image_state(const std::string& source);
// The decoded image's natural size (0 x 0 until it has loaded).
void image_natural_size(const std::string& source, int& w, int& h);

// Registers bytes for a source that is not a URL or a file (an image inside an
// EPUB or a comic archive). `loader` runs on a worker thread.
void image_register_loader(const std::string& source, std::function<bool(std::string&)> loader);
// Registers a loader for every source starting with `prefix` (images inside
// archives: "archive:" + file + '\x01' + inner path). Runs on a worker thread.
void image_register_prefix_loader(const std::string& prefix,
                                  std::function<bool(const std::string& source, std::string& bytes)> loader);
// Drops every cached image whose source starts with `prefix` (a closed book).
void images_forget(const std::string& prefix);
// Lets images under `prefix` that failed to load be tried again on their next
// request (a drive that was unplugged has come back). Loaded ones are kept.
void images_retry_failed(const std::string& prefix);
size_t images_memory();

}  // namespace gfx
