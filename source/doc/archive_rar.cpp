// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// RAR archives (CBR comics), RAR4 and RAR5, through libarchive's readers.
// libarchive reads an archive front to back, so one reader is kept open and
// moved forward: reading pages in order costs nothing extra, and going back
// reopens the file. (Solid archives must be decompressed up to a page anyway.)
#include <pthread.h>
#include <string.h>

#include <memory>
#include <string>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/archive.h"

extern "C" {
#include "libarchive/archive.h"
#include "libarchive/archive_entry.h"
}

namespace doc {

namespace {

class RarArchive : public Archive {
public:
    ~RarArchive() override {
        close_reader();
        pthread_mutex_destroy(&lock_);
    }

    bool open(const std::string& path, std::string* error) {
        path_ = path;
        if (!reopen()) {
            if (error) *error = "Not a ZIP or RAR archive, or it is damaged.";
            return false;
        }
        archive_entry* entry;
        int rc;
        while ((rc = archive_read_next_header(reader_, &entry)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
            const char* name = archive_entry_pathname_utf8(entry);
            if (!name) name = archive_entry_pathname(entry);
            entries_.push_back({name ? name : "", (uint64_t)std::max<la_int64_t>(0, archive_entry_size(entry)),
                                archive_entry_filetype(entry) == AE_IFDIR});
            archive_read_data_skip(reader_);
        }
        if (rc != ARCHIVE_EOF && entries_.empty()) {
            if (error) *error = std::string("The RAR archive could not be read: ") + archive_error_string(reader_);
            close_reader();
            return false;
        }
        close_reader();
        LOG("rar: %zu entries", entries_.size());
        return true;
    }

    const std::vector<ArchiveEntry>& entries() const override { return entries_; }

    bool read(const std::string& name, std::string& out, uint64_t max_bytes) override {
        out.clear();
        int index = -1;
        for (size_t i = 0; i < entries_.size() && index < 0; ++i)
            if (entries_[i].name == name) index = (int)i;
        for (size_t i = 0; i < entries_.size() && index < 0; ++i)
            if (lib::iequals(entries_[i].name, name)) index = (int)i;
        if (index < 0 || entries_[index].size > max_bytes) return false;

        pthread_mutex_lock(&lock_);
        bool ok = seek_to(index);
        if (ok) {
            out.reserve(entries_[index].size);
            char buffer[64 * 1024];
            la_ssize_t n;
            while ((n = archive_read_data(reader_, buffer, sizeof buffer)) > 0) {
                out.append(buffer, (size_t)n);
                if (out.size() > max_bytes) break;
            }
            ok = n == 0 && out.size() <= max_bytes;
            if (n < 0) LOG("rar: %s", archive_error_string(reader_));
            // The data was consumed: the reader now stands before entry index + 1.
            next_ = index + 1;
        }
        if (!ok) close_reader();
        pthread_mutex_unlock(&lock_);
        return ok;
    }

private:
    bool reopen() {
        close_reader();
        reader_ = archive_read_new();
        archive_read_support_format_rar(reader_);
        archive_read_support_format_rar5(reader_);
        // Through lib::open_file, so a drive read over FTP works too.
        file_ = lib::open_file(path_);
        if (!file_) {
            close_reader();
            return false;
        }
        archive_read_set_callback_data(reader_, this);
        archive_read_set_read_callback(reader_, &RarArchive::on_read);
        archive_read_set_skip_callback(reader_, &RarArchive::on_skip);
        archive_read_set_seek_callback(reader_, &RarArchive::on_seek);
        if (archive_read_open1(reader_) != ARCHIVE_OK) {
            close_reader();
            return false;
        }
        next_ = 0;
        return true;
    }

    static la_ssize_t on_read(struct archive*, void* self, const void** block) {
        RarArchive* r = static_cast<RarArchive*>(self);
        *block = r->block_;
        const size_t n = fread(r->block_, 1, sizeof r->block_, r->file_);
        return n == 0 && ferror(r->file_) ? -1 : (la_ssize_t)n;
    }
    static la_int64_t on_skip(struct archive*, void* self, la_int64_t count) {
        RarArchive* r = static_cast<RarArchive*>(self);
        return fseeko(r->file_, (off_t)count, SEEK_CUR) == 0 ? count : 0;
    }
    static la_int64_t on_seek(struct archive*, void* self, la_int64_t offset, int whence) {
        RarArchive* r = static_cast<RarArchive*>(self);
        if (fseeko(r->file_, (off_t)offset, whence) != 0) return ARCHIVE_FATAL;
        return (la_int64_t)ftello(r->file_);
    }

    void close_reader() {
        if (reader_) archive_read_free(reader_);
        reader_ = nullptr;
        if (file_) fclose(file_);
        file_ = nullptr;
        next_ = 0;
    }

    // Positions the reader at the start of entry `index`'s data.
    bool seek_to(int index) {
        if (!reader_ || index < next_) {
            if (!reopen()) return false;
        }
        archive_entry* entry;
        while (next_ <= index) {
            const int rc = archive_read_next_header(reader_, &entry);
            if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) return false;
            if (next_ == index) return true;
            archive_read_data_skip(reader_);
            ++next_;
        }
        return false;
    }

    std::string path_;
    std::vector<ArchiveEntry> entries_;
    struct archive* reader_ = nullptr;
    FILE* file_ = nullptr;
    char block_[256 * 1024];
    int next_ = 0;  // index of the entry whose header comes next
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
};

}  // namespace

std::shared_ptr<Archive> open_rar_archive(const std::string& path, std::string* error) {
    auto rar = std::make_shared<RarArchive>();
    if (!rar->open(path, error)) return nullptr;
    return rar;
}

}  // namespace doc
