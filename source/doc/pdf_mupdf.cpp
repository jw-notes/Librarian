// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// PDF through MuPDF (AGPL-3.0). Linked only when MuPDF's libraries have been
// built (tools/build_mupdf.sh); otherwise pdf_none.cpp is used.
#include <pthread.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/fs.h"
#include "core/log.h"
#include "core/usb_ftp.h"
#include "doc/paged.h"

extern "C" {
#include <mupdf/fitz.h>
}

namespace doc {

namespace {

// MuPDF keeps some state shared by every context (its JPEG 2000 decoder's
// allocator, for one), guarded only by the lock callbacks a context is given.
// Without them, two documents decoding JPEG 2000 images on two threads (two
// cover loads, or a cover load and the reader) crashed: a USB drive of NASA
// PDFs did it on the console. Every context shares these process-wide locks.
pthread_mutex_t g_fz_mutex[FZ_LOCK_MAX];
pthread_once_t g_fz_once = PTHREAD_ONCE_INIT;

void fz_lock_fn(void*, int lock) { pthread_mutex_lock(&g_fz_mutex[lock]); }
void fz_unlock_fn(void*, int lock) { pthread_mutex_unlock(&g_fz_mutex[lock]); }

fz_locks_context* fz_locks() {
    static fz_locks_context locks = {nullptr, fz_lock_fn, fz_unlock_fn};
    pthread_once(&g_fz_once, [] {
        for (auto& m : g_fz_mutex) pthread_mutex_init(&m, nullptr);
    });
    return &locks;
}

// One context per document, and every call on it made under the document's
// mutex; what contexts share is guarded by fz_locks().
class PdfDocument : public PagedDocument {
public:
    ~PdfDocument() override {
        if (ctx_) {
            if (doc_) fz_drop_document(ctx_, doc_);
            fz_drop_context(ctx_);
        }
        if (file_) fclose(file_);
        pthread_mutex_destroy(&lock_);
    }

    bool open(const std::string& path, std::string& error, int store_mb) {
        ctx_ = fz_new_context(nullptr, fz_locks(), (size_t)store_mb << 20);
        if (!ctx_) {
            error = "Not enough memory to open the PDF.";
            return false;
        }
        bool ok = true;
        fz_try(ctx_) {
            fz_register_document_handlers(ctx_);
            if (lib::usb_ftp::handles(path)) {
                // A drive read through GoldHEN's FTP server: MuPDF reads it
                // through our stream (kept open until the document is dropped).
                file_ = lib::open_file(path);
                if (!file_) fz_throw(ctx_, FZ_ERROR_GENERIC, "The file could not be opened.");
                fz_stream* stream = fz_open_file_ptr_no_close(ctx_, file_);
                fz_try(ctx_) doc_ = fz_open_document_with_stream(ctx_, path.c_str(), stream);
                fz_always(ctx_) fz_drop_stream(ctx_, stream);
                fz_catch(ctx_) fz_rethrow(ctx_);
            } else {
                doc_ = fz_open_document(ctx_, path.c_str());
            }
            if (fz_needs_password(ctx_, doc_) && !fz_authenticate_password(ctx_, doc_, ""))
                fz_throw(ctx_, FZ_ERROR_GENERIC, "This PDF is protected by a password.");
            pages_ = fz_count_pages(ctx_, doc_);
            char buffer[512];
            if (fz_lookup_metadata(ctx_, doc_, FZ_META_INFO_TITLE, buffer, sizeof buffer) > 0) title_ = buffer;
            if (fz_lookup_metadata(ctx_, doc_, FZ_META_INFO_AUTHOR, buffer, sizeof buffer) > 0) author_ = buffer;
        }
        fz_catch(ctx_) {
            error = fz_caught_message(ctx_);
            // Handled here: MuPDF 1.24 otherwise reports it again ("UNHANDLED
            // EXCEPTION") on the next error or when the context is dropped.
            fz_ignore_error(ctx_);
            ok = false;
        }
        if (ok && pages_ <= 0) {
            error = "The PDF has no pages.";
            ok = false;
        }
        // A file that cannot be opened (a drive unplugged) is not a PDF of no pages.
        if (ok) LOG("pdf: %d pages", pages_);
        else LOG("pdf: not opened: %s", error.c_str());
        return ok;
    }

    int page_count() const override { return pages_; }
    bool is_pdf() const override { return true; }
    std::string title() const override { return title_; }
    std::string author() const override { return author_; }

    bool render(int index, int max_w, int max_h, gfx::Image& out, std::string& error) override {
        if (index < 0 || index >= pages_) return false;
        pthread_mutex_lock(&lock_);
        fz_page* page = nullptr;
        fz_pixmap* pix = nullptr;
        bool ok = true;
        fz_var(page);
        fz_var(pix);
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, index);
            const fz_rect bounds = fz_bound_page(ctx_, page);
            const float w = std::max(1.f, bounds.x1 - bounds.x0), h = std::max(1.f, bounds.y1 - bounds.y0);
            const float zoom = std::min(max_w / w, max_h / h);
            pix = fz_new_pixmap_from_page(ctx_, page, fz_scale(zoom, zoom), fz_device_rgb(ctx_), 0);
            const int pw = fz_pixmap_width(ctx_, pix), ph = fz_pixmap_height(ctx_, pix);
            const int n = fz_pixmap_components(ctx_, pix), stride = (int)fz_pixmap_stride(ctx_, pix);
            const unsigned char* samples = fz_pixmap_samples(ctx_, pix);
            out.w = pw;
            out.h = ph;
            out.opaque = true;
            out.px.resize((size_t)pw * ph);
            for (int y = 0; y < ph; ++y) {
                const unsigned char* row = samples + (size_t)y * stride;
                uint32_t* dst = out.px.data() + (size_t)y * pw;
                for (int x = 0; x < pw; ++x, row += n) dst[x] = 0xFF000000u | (row[0] << 16) | (row[1] << 8) | row[2];
            }
        }
        fz_always(ctx_) {
            fz_drop_pixmap(ctx_, pix);
            fz_drop_page(ctx_, page);
        }
        fz_catch(ctx_) {
            error = std::string("Page ") + std::to_string(index + 1) + ": " + fz_caught_message(ctx_);
            fz_ignore_error(ctx_);
            ok = false;
        }
        pthread_mutex_unlock(&lock_);
        return ok;
    }

    PageInfo page_info(int index) override {
        PageInfo info;
        if (index < 0 || index >= pages_) return info;
        pthread_mutex_lock(&lock_);
        fz_page* page = nullptr;
        fz_var(page);
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, index);
            const fz_rect b = fz_bound_page(ctx_, page);
            info = {b.x1 - b.x0, b.y1 - b.y0};
        }
        fz_always(ctx_) { fz_drop_page(ctx_, page); }
        fz_catch(ctx_) { fz_ignore_error(ctx_); info = {}; }
        pthread_mutex_unlock(&lock_);
        return info;
    }

    std::vector<PageLink> links(int index) override {
        std::vector<PageLink> links;
        if (index < 0 || index >= pages_) return links;
        pthread_mutex_lock(&lock_);
        fz_page* page = nullptr;
        fz_link* root = nullptr;
        fz_var(page); fz_var(root);
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, index);
            const fz_rect bounds = fz_bound_page(ctx_, page);
            root = fz_load_links(ctx_, page);
            for (fz_link* l = root; l; l = l->next) {
                PageLink link;
                link.x = l->rect.x0 - bounds.x0; link.y = l->rect.y0 - bounds.y0;
                link.w = l->rect.x1 - l->rect.x0; link.h = l->rect.y1 - l->rect.y0;
                if (l->uri) link.uri = l->uri;
                if (l->uri && !fz_is_external_link(ctx_, l->uri)) {
                    float x = 0, y = 0;
                    const fz_location dest = fz_resolve_link(ctx_, doc_, l->uri, &x, &y);
                    link.page = fz_page_number_from_location(ctx_, doc_, dest);
                    link.dest_x = std::isfinite(x) ? x : 0;
                    link.dest_y = std::isfinite(y) ? y : 0;
                }
                links.push_back(std::move(link));
            }
        }
        fz_always(ctx_) { fz_drop_link(ctx_, root); fz_drop_page(ctx_, page); }
        fz_catch(ctx_) { fz_ignore_error(ctx_); links.clear(); }
        pthread_mutex_unlock(&lock_);
        return links;
    }

    bool render_region(int index, float scale, int rotation, const gfx::Rect& region,
                       gfx::Image& out, std::string& error) override {
        if (index < 0 || index >= pages_ || !std::isfinite(scale) || scale <= 0 || scale > 32 ||
            region.empty() || region.w > 1024 || region.h > 1024) {
            error = "Invalid PDF tile size.";
            return false;
        }
        pthread_mutex_lock(&lock_);
        fz_page* page = nullptr;
        fz_pixmap* pix = nullptr;
        fz_device* dev = nullptr;
        bool ok = true;
        fz_var(page); fz_var(pix); fz_var(dev);
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, index);
            const fz_rect bounds = fz_bound_page(ctx_, page);
            fz_matrix matrix = fz_pre_rotate(fz_scale(scale, scale), rotation);
            const fz_rect transformed = fz_transform_rect(bounds, matrix);
            matrix = fz_concat(matrix, fz_translate(-transformed.x0, -transformed.y0));
            const fz_irect box = {region.x, region.y, region.right(), region.bottom()};
            pix = fz_new_pixmap_with_bbox(ctx_, fz_device_rgb(ctx_), box, nullptr, 0);
            fz_clear_pixmap_with_value(ctx_, pix, 255);
            dev = fz_new_draw_device(ctx_, fz_identity, pix);
            fz_run_page(ctx_, page, dev, matrix, nullptr);
            fz_close_device(ctx_, dev);
            out.w = fz_pixmap_width(ctx_, pix); out.h = fz_pixmap_height(ctx_, pix);
            out.opaque = true;
            out.px.resize((size_t)out.w * out.h);
            const int n = fz_pixmap_components(ctx_, pix), stride = (int)fz_pixmap_stride(ctx_, pix);
            const unsigned char* samples = fz_pixmap_samples(ctx_, pix);
            for (int y = 0; y < out.h; ++y) {
                const unsigned char* row = samples + (size_t)y * stride;
                for (int x = 0; x < out.w; ++x, row += n)
                    out.px[(size_t)y * out.w + x] = 0xFF000000u | (row[0] << 16) | (row[1] << 8) | row[2];
            }
        }
        fz_always(ctx_) {
            fz_drop_device(ctx_, dev); fz_drop_pixmap(ctx_, pix); fz_drop_page(ctx_, page);
        }
        fz_catch(ctx_) { error = fz_caught_message(ctx_); fz_ignore_error(ctx_); ok = false; }
        pthread_mutex_unlock(&lock_);
        return ok;
    }

    std::vector<OutlineEntry> outline() override {
        std::vector<OutlineEntry> entries;
        pthread_mutex_lock(&lock_);
        fz_outline* root = nullptr;
        fz_var(root);
        fz_try(ctx_) {
            root = fz_load_outline(ctx_, doc_);
            std::function<void(fz_outline*, int)> walk = [&](fz_outline* o, int level) {
                for (; o; o = o->next) {
                    if (o->title) entries.push_back({o->title, std::max(0, fz_page_number_from_location(ctx_, doc_, o->page)), level});
                    walk(o->down, level + 1);
                }
            };
            walk(root, 1);
        }
        fz_always(ctx_) { fz_drop_outline(ctx_, root); }
        fz_catch(ctx_) { fz_ignore_error(ctx_); entries.clear(); }
        pthread_mutex_unlock(&lock_);
        return entries;
    }

private:
    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    int pages_ = 0;
    std::string title_, author_;
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
    FILE* file_ = nullptr;  // the stream of a PDF read over FTP
};

}  // namespace

std::shared_ptr<PagedDocument> open_pdf(const std::string& path, std::string& error, int store_mb) {
    auto pdf = std::make_shared<PdfDocument>();
    if (!pdf->open(path, error, store_mb)) return nullptr;
    return pdf;
}

}  // namespace doc
