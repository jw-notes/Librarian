// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// MOBI / AZW3 reading. The format is documented by the MobileRead wiki
// ("MOBI" and "KF8") and by the open-source KindleUnpack and Calibre readers,
// whose algorithms (trailing-entry trimming, HUFF/CDIC, KF8 reassembly) this
// follows.
#include "doc/mobi.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>

#include "core/fs.h"
#include "core/log.h"
#include "core/markup.h"
#include "core/text.h"
#include "doc/html_convert.h"
#include "gfx/image_cache.h"

namespace doc {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFF;

uint32_t be32(const std::string& s, size_t at) {
    if (at + 4 > s.size()) return 0;
    const unsigned char* p = (const unsigned char*)s.data() + at;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint16_t be16(const std::string& s, size_t at) {
    if (at + 2 > s.size()) return 0;
    const unsigned char* p = (const unsigned char*)s.data() + at;
    return (uint16_t)((p[0] << 8) | p[1]);
}

// ---- decompression ---------------------------------------------------------------------

std::string palmdoc_decompress(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 2);
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i++];
        if (c >= 1 && c <= 8) {
            out.append(in, i, std::min<size_t>(c, in.size() - i));
            i += c;
        } else if (c < 0x80) {
            out += (char)c;
        } else if (c >= 0xC0) {
            out += ' ';
            out += (char)(c ^ 0x80);
        } else if (i < in.size()) {
            const unsigned m = ((unsigned)c << 8) | (unsigned char)in[i++];
            const size_t distance = (m >> 3) & 0x7FF, length = (m & 7) + 3;
            if (distance == 0 || distance > out.size()) continue;
            const size_t from = out.size() - distance;
            for (size_t k = 0; k < length; ++k) out += out[from + k];  // may overlap
        }
    }
    return out;
}

// HUFF/CDIC, the compression used by Amazon's tools (kindlegen).
class HuffCdic {
public:
    bool load(const std::string& huff, const std::vector<std::string>& cdics) {
        if (huff.compare(0, 8, std::string("HUFF\0\0\0\x18", 8)) != 0) return false;
        const uint32_t off1 = be32(huff, 8), off2 = be32(huff, 12);
        if ((uint64_t)off1 + 256 * 4 > huff.size() || (uint64_t)off2 + 32 * 8 > huff.size()) return false;
        for (int i = 0; i < 256; ++i) {
            const uint32_t v = be32(huff, off1 + i * 4);
            Entry& e = dict1_[i];
            e.codelen = v & 0x1F;
            e.term = (v & 0x80) != 0;
            uint64_t maxcode = v >> 8;
            if (e.codelen == 0) return false;
            if (e.codelen <= 8 && !e.term) return false;
            e.maxcode = ((maxcode + 1) << (32 - e.codelen)) - 1;
        }
        for (int codelen = 1; codelen <= 32; ++codelen) {
            mincode_[codelen] = (uint64_t)be32(huff, off2 + (codelen - 1) * 8) << (32 - codelen);
            maxcode_[codelen] = (((uint64_t)be32(huff, off2 + (codelen - 1) * 8 + 4) + 1) << (32 - codelen)) - 1;
        }
        for (const std::string& cdic : cdics) {
            if (cdic.compare(0, 8, std::string("CDIC\0\0\0\x10", 8)) != 0) return false;
            const uint32_t phrases = be32(cdic, 8), bits = be32(cdic, 12);
            if (bits > 16 || phrases < phrases_.size()) return false;
            const size_t n = std::min<size_t>((size_t)1 << bits, phrases - phrases_.size());
            if (16 + n * 2 > cdic.size()) return false;
            for (size_t i = 0; i < n; ++i) {
                const uint16_t off = be16(cdic, 16 + i * 2);
                const uint16_t blen = be16(cdic, 16 + off);
                if (18u + off + (blen & 0x7FFFu) > cdic.size()) return false;
                phrases_.push_back({cdic.substr(16 + off + 2, blen & 0x7FFF), (blen & 0x8000) != 0});
            }
        }
        return !phrases_.empty();
    }

    std::string unpack(const std::string& input, int depth = 0) {
        std::string out;
        if (depth > 32) return out;
        std::string data = input + std::string(8, '\0');
        int64_t bitsleft = (int64_t)input.size() * 8;
        size_t pos = 0;
        auto read64 = [&](size_t at) {
            uint64_t x = 0;
            for (int k = 0; k < 8; ++k) x = (x << 8) | (unsigned char)data[at + k];
            return x;
        };
        uint64_t x = read64(0);
        int n = 32;
        for (;;) {
            if (n <= 0) {
                pos += 4;
                if (pos + 8 > data.size()) break;
                x = read64(pos);
                n += 32;
            }
            const uint64_t code = (x >> n) & 0xFFFFFFFFull;
            const Entry& e = dict1_[code >> 24];
            uint32_t codelen = e.codelen;
            uint64_t maxcode = e.maxcode;
            if (!e.term) {
                while (codelen < 32 && code < mincode_[codelen]) ++codelen;
                maxcode = maxcode_[codelen];
            }
            n -= (int)codelen;
            bitsleft -= codelen;
            if (bitsleft < 0) break;
            const uint64_t index = (maxcode - code) >> (32 - codelen);
            if (index >= phrases_.size()) break;
            Phrase& p = phrases_[index];
            if (!p.expanded) {
                p.text = unpack(p.text, depth + 1);
                p.expanded = true;
            }
            out += p.text;
        }
        return out;
    }

private:
    struct Entry { uint32_t codelen = 0; bool term = false; uint64_t maxcode = 0; };
    struct Phrase { std::string text; bool expanded; };
    Entry dict1_[256];
    uint64_t mincode_[33] = {}, maxcode_[33] = {};
    std::vector<Phrase> phrases_;
};

// Bytes appended to each text record (multibyte overlaps, indexing data).
size_t trailing_size(const std::string& data, size_t size) {
    // A backward-encoded variable-width number at the end.
    uint32_t num = 0;
    for (size_t i = size >= 4 ? size - 4 : 0; i < size; ++i) {
        const unsigned char v = (unsigned char)data[i];
        if (v & 0x80) num = 0;
        num = (num << 7) | (v & 0x7F);
    }
    return num;
}

size_t trimmed_length(const std::string& data, uint32_t flags) {
    size_t size = data.size();
    for (uint32_t f = flags >> 1; f; f >>= 1) {
        if (f & 1) {
            const size_t n = trailing_size(data, size);
            size = n <= size ? size - n : 0;
        }
    }
    if ((flags & 1) && size > 0) {
        const size_t n = ((unsigned char)data[size - 1] & 3) + 1;
        size = n <= size ? size - n : 0;
    }
    return size;
}

// ---- KF8 indexes (INDX records with a TAGX table) -----------------------------------------

uint32_t varint_forward(const std::string& data, size_t& pos) {
    uint32_t value = 0;
    while (pos < data.size()) {
        const unsigned char v = (unsigned char)data[pos++];
        value = (value << 7) | (v & 0x7F);
        if (v & 0x80) break;
    }
    return value;
}

struct IndexEntry {
    std::string name;
    std::map<int, std::vector<uint32_t>> tags;
};

struct Tag { int tag, per_entry, mask, end; };

int bit_count(int v) {
    int n = 0;
    for (; v; v >>= 1) n += v & 1;
    return n;
}

std::vector<IndexEntry> read_index(const std::function<std::string(uint32_t)>& rec, uint32_t first,
                                   std::map<uint32_t, std::string>* cncx) {
    std::vector<IndexEntry> out;
    if (first == kNone) return out;
    const std::string head = rec(first);
    if (head.compare(0, 4, "INDX") != 0) return out;
    const uint32_t header_len = be32(head, 4), records = be32(head, 24), cncx_count = be32(head, 52);
    if (header_len > head.size() || records > 65535 || cncx_count > 65535) return out;
    // The TAGX table describing each entry's tags.
    std::vector<Tag> table;
    uint32_t control_bytes = 1;
    if (head.compare(header_len, 4, "TAGX") == 0) {
        const uint32_t first_entry = be32(head, header_len + 4);
        control_bytes = be32(head, header_len + 8);
        if (first_entry < 12 || (uint64_t)header_len + first_entry > head.size() || control_bytes > 256) return out;
        for (uint32_t i = 12; i + 4 <= first_entry; i += 4) {
            const size_t p = header_len + i;
            table.push_back({(unsigned char)head[p], (unsigned char)head[p + 1], (unsigned char)head[p + 2],
                             (unsigned char)head[p + 3]});
        }
    }
    // CNCX records: labels referenced by offset.
    if (cncx) {
        uint32_t base = 0;
        for (uint32_t j = 0; j < cncx_count; ++j) {
            const std::string c = rec(first + records + 1 + j);
            size_t pos = 0;
            while (pos < c.size() && c[pos] != 0) {
                const size_t at = pos;
                const uint32_t len = varint_forward(c, pos);
                (*cncx)[base + (uint32_t)at] = c.substr(pos, len);
                pos += len;
            }
            base += 0x10000;
        }
    }
    for (uint32_t r = 1; r <= records; ++r) {
        const std::string d = rec(first + r);
        if (d.compare(0, 4, "INDX") != 0) continue;
        const uint32_t idxt = be32(d, 20), count = be32(d, 24);
        if ((uint64_t)idxt + 4 + (uint64_t)count * 2 > d.size()) continue;
        std::vector<uint32_t> starts;
        for (uint32_t j = 0; j < count; ++j) starts.push_back(be16(d, idxt + 4 + 2 * j));
        starts.push_back(idxt);
        for (uint32_t j = 0; j < count; ++j) {
            const size_t start = starts[j], end = starts[j + 1];
            if (start >= end || end > d.size()) break;
            IndexEntry e;
            const unsigned len = (unsigned char)d[start];
            e.name = d.substr(start + 1, len);
            const size_t control = start + 1 + len;
            if (control + control_bytes > end) continue;
            size_t pos = control + control_bytes;
            // Which tags are present, and how many values each has.
            struct Present { int tag, count, bytes, per_entry; };
            std::vector<Present> present;
            size_t cb = 0;
            for (const Tag& t : table) {
                if (t.end == 1) {
                    ++cb;
                    continue;
                }
                if (control + cb >= d.size()) break;
                int value = (unsigned char)d[control + cb] & t.mask;
                if (!value) continue;
                if (value == t.mask && bit_count(t.mask) > 1) {
                    present.push_back({t.tag, -1, (int)varint_forward(d, pos), t.per_entry});
                } else {
                    int mask = t.mask;
                    while (!(mask & 1)) {
                        mask >>= 1;
                        value >>= 1;
                    }
                    present.push_back({t.tag, value, 0, t.per_entry});
                }
            }
            for (const Present& p : present) {
                std::vector<uint32_t>& values = e.tags[p.tag];
                if (p.count >= 0) {
                    for (int k = 0; k < p.count * p.per_entry && pos < end; ++k) values.push_back(varint_forward(d, pos));
                } else {
                    const size_t stop = pos + (size_t)p.bytes;
                    while (pos < stop && pos < end) values.push_back(varint_forward(d, pos));
                }
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

uint32_t base32(const std::string& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 32); }

// Shared, so image loaders can read pictures of books that are open.
std::mutex g_open_lock;
std::map<std::string, std::weak_ptr<const std::string>> g_open_files;

}  // namespace

std::string Mobi::image_key(uint32_t record) const { return "mobi:" + path_ + '\x01' + std::to_string(record); }

bool Mobi::record(uint32_t index, std::string& bytes) const {
    if (!data_ || index >= offsets_.size()) return false;
    const uint32_t start = offsets_[index], end = index + 1 < offsets_.size() ? offsets_[index + 1] : (uint32_t)data_->size();
    if (start > end || end > data_->size()) return false;
    bytes.assign(*data_, start, end - start);
    return true;
}

bool Mobi::open(const std::string& path, std::string& error) {
    *this = Mobi();
    path_ = path;
    auto data = std::make_shared<std::string>();
    if (!lib::read_file(path, *data, 512ull * 1024 * 1024)) {
        error = "The file could not be read.";
        return false;
    }
    data_ = data;
    const std::string& d = *data;
    if (d.size() < 78) {
        error = "The file is too short to be a Kindle book.";
        return false;
    }
    if (d.compare(0, 4, "TPZ0") == 0 || d.compare(60, 4, "TPZ0") == 0) {
        error = "This is a Topaz Kindle book, a format only Amazon's own readers open.";
        return false;
    }
    if (d.compare(60, 8, "BOOKMOBI") != 0 && d.compare(60, 8, "TEXtREAd") != 0) {
        error = "This is not a MOBI or AZW3 book.";
        return false;
    }
    const uint16_t count = be16(d, 76);
    if (78u + (size_t)count * 8 > d.size()) {
        error = "The book's record table is truncated.";
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t offset = be32(d, 78 + i * 8);
        if (offset < 78u + (size_t)count * 8 || offset > d.size() || (!offsets_.empty() && offset < offsets_.back())) {
            error = "The book's record offsets are invalid.";
            return false;
        }
        offsets_.push_back(offset);
    }
    if (offsets_.empty()) {
        error = "The book has no records.";
        return false;
    }
    // Picks the KF8 part of a joint MOBI/KF8 file when there is one.
    auto parse_header = [&](uint32_t base) -> bool {
        std::string r0;
        if (!record(base, r0) || r0.size() < 16) return false;
        compression_ = be16(r0, 0);
        text_length_ = be32(r0, 4);
        text_records_ = be16(r0, 8);
        if (be16(r0, 12) != 0) {
            error = "This book is protected by DRM (copy protection). Librarian can only open DRM-free Kindle books.";
            return false;
        }
        if (r0.compare(16, 4, "MOBI") != 0) {
            // A plain PalmDOC: no MOBI header.
            encoding_ = 1252;
            return true;
        }
        const uint32_t hlen = be32(r0, 20);
        if (hlen < 0x74 || hlen > r0.size() - 16) {
            error = "The book's MOBI header is truncated.";
            return false;
        }
        author_.clear();
        cover_offset_ = -1;
        encoding_ = be32(r0, 28);
        const uint32_t version = be32(r0, 36);
        kf8_ = version >= 8;
        first_resource_ = be32(r0, 0x6C);
        huff_record_ = be32(r0, 0x70);
        huff_count_ = be32(r0, 0x74);
        const uint32_t exth_flags = be32(r0, 0x80);
        if (hlen + 16 >= 0xF4) extra_flags_ = be16(r0, 0xF2);
        if (kf8_) {
            fdst_ = be32(r0, 0xC0);
            ncx_index_ = be32(r0, 0xF4);
            fragment_index_ = be32(r0, 0xF8);
            skeleton_index_ = be32(r0, 0xFC);
        } else if (hlen + 16 >= 0xF8) {
            ncx_index_ = be32(r0, 0xF4);
        }
        // Record numbers in the KF8 part of a joint file count from its own record 0.
        auto rebase = [&](uint32_t& v) {
            if (v != kNone && base) v += base;
        };
        rebase(fdst_);
        rebase(ncx_index_);
        rebase(fragment_index_);
        rebase(skeleton_index_);
        rebase(huff_record_);
        // The full title.
        const uint32_t name_off = be32(r0, 0x54), name_len = be32(r0, 0x58);
        if (name_off && name_off + name_len <= r0.size()) title_ = r0.substr(name_off, name_len);
        // EXTH: author, title, cover, the KF8 boundary.
        if (exth_flags & 0x40) {
            const size_t ex = 16 + hlen;
            if (r0.compare(ex, 4, "EXTH") == 0) {
                const uint32_t n = be32(r0, ex + 8);
                size_t p = ex + 12;
                for (uint32_t k = 0; k < n && p + 8 <= r0.size(); ++k) {
                    const uint32_t type = be32(r0, p), len = be32(r0, p + 4);
                    if (len < 8 || p + len > r0.size()) break;
                    const std::string value = r0.substr(p + 8, len - 8);
                    if (type == 100) author_ += (author_.empty() ? "" : ", ") + value;
                    else if (type == 503) title_ = value;
                    else if (type == 201 && value.size() >= 4) cover_offset_ = (int32_t)be32(value, 0);
                    else if (type == 121 && value.size() >= 4 && base == 0) {
                        const uint32_t boundary = be32(value, 0);
                        if (boundary != kNone && boundary < offsets_.size()) kf8_boundary_hint_ = boundary;
                    }
                    p += len;
                }
            }
        }
        return true;
    };
    kf8_boundary_hint_ = kNone;
    if (!parse_header(0)) return false;
    if (!kf8_ && kf8_boundary_hint_ != kNone) {
        // A joint file: its KF8 part keeps more of the book's formatting.
        const std::string author = author_;
        const int32_t cover = cover_offset_;
        const uint32_t first_resource = first_resource_;
        if (!parse_header(kf8_boundary_hint_)) return false;
        {
            base_ = kf8_boundary_hint_;
            if (author_.empty()) author_ = author;
            if (cover_offset_ < 0) cover_offset_ = cover;
            // Pictures are shared by both parts and numbered from the first part.
            first_resource_ = first_resource;
        }
    }
    title_ = lib::to_utf8(title_);
    author_ = lib::to_utf8(author_);
    if (compression_ != 1 && compression_ != 2 && compression_ != 17480) {
        error = "This book uses a compression Librarian does not know (" + std::to_string(compression_) + ").";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_open_lock);
        g_open_files[path_] = data_;
    }
    return true;
}

bool Mobi::cover(std::string& bytes) const {
    if (cover_offset_ < 0 || first_resource_ == kNone) return false;
    return record(first_resource_ + (uint32_t)cover_offset_, bytes);
}

bool Mobi::decode_text(std::string& text, std::string& error) {
    HuffCdic huff;
    if (compression_ == 17480) {
        std::string h;
        std::vector<std::string> cdics;
        if (!record(huff_record_, h)) {
            error = "The book's dictionary is missing.";
            return false;
        }
        for (uint32_t i = 1; i < huff_count_; ++i) {
            std::string c;
            if (record(huff_record_ + i, c)) cdics.push_back(std::move(c));
        }
        if (!huff.load(h, cdics)) {
            error = "The book's dictionary could not be read.";
            return false;
        }
    }
    text.clear();
    if (text_length_ > 256u * 1024 * 1024 || text_records_ >= offsets_.size() - base_) {
        error = "The book's text size or record count is invalid.";
        return false;
    }
    text.reserve(text_length_);
    for (uint32_t i = 1; i <= text_records_; ++i) {
        std::string r;
        if (!record(base_ + i, r)) break;
        r.resize(trimmed_length(r, extra_flags_));
        if (compression_ == 2) text += palmdoc_decompress(r);
        else if (compression_ == 17480) text += huff.unpack(r);
        else text += r;
    }
    if (text_length_ && text.size() > text_length_) text.resize(text_length_);
    if (text.empty()) {
        error = "The book contains no text.";
        return false;
    }
    return true;
}

bool Mobi::prepare(std::string& error) {
    if (prepared_) return !sections_.empty();
    prepared_ = true;
    std::string text;
    if (!decode_text(text, error)) return false;
    if (kf8_) build_kf8(text);
    else build_mobi6(text);
    if (sections_.empty()) {
        error = "The book's text could not be rebuilt.";
        return false;
    }
    read_ncx();
    LOG("mobi: %s, %zu sections, %zu contents entries", kf8_ ? "KF8" : "MOBI 6", sections_.size(), toc_.size());
    return true;
}

// MOBI 6: one HTML stream. Links are byte offsets (filepos=), pictures are
// record numbers (recindex=). Anchors are inserted at every linked offset, and
// the stream is cut into sections at page breaks.
void Mobi::build_mobi6(std::string& text) {
    std::vector<uint32_t> targets;
    for (size_t at = 0; (at = text.find("filepos=", at)) != std::string::npos;) {
        at += 8;
        while (at < text.size() && (text[at] == '"' || text[at] == '\'')) ++at;
        targets.push_back((uint32_t)strtoul(text.c_str() + at, nullptr, 10));
    }
    auto rec = [this](uint32_t i) { std::string r; record(i, r); return r; };
    for (const auto& e : read_index(rec, ncx_index_, nullptr)) {
        auto pos = e.tags.find(1);
        if (pos != e.tags.end() && !pos->second.empty()) targets.push_back(pos->second[0]);
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    // Section cuts at page breaks, at least ~40 KB apart.
    std::vector<uint32_t> cuts = {0};
    for (size_t at = 0; (at = text.find("<mbp:pagebreak", at)) != std::string::npos; at += 14)
        if (at - cuts.back() > 40 * 1024) cuts.push_back((uint32_t)at);
    // Insert anchors back to front so earlier offsets stay valid.
    std::vector<std::pair<uint32_t, std::string>> inserts;
    for (uint32_t t : targets)
        if (t < text.size()) inserts.push_back({t, "<a id=\"filepos" + std::to_string(t) + "\"></a>"});
    std::string with;
    with.reserve(text.size() + inserts.size() * 24);
    size_t prev = 0, ins = 0, cut = 1;
    section_start_.clear();
    // Walk the text once: anchors in place, sections split at the cuts.
    std::string current;
    for (size_t pos = 0; pos <= text.size(); ++pos) {
        if (cut < cuts.size() && pos == cuts[cut]) {
            current.append(text, prev, pos - prev);
            prev = pos;
            sections_.push_back(std::move(current));
            current.clear();
            ++cut;
        }
        while (ins < inserts.size() && inserts[ins].first == pos) {
            current.append(text, prev, pos - prev);
            prev = pos;
            current += inserts[ins].second;
            ++ins;
        }
    }
    current.append(text, prev, text.size() - prev);
    sections_.push_back(std::move(current));
    section_start_ = cuts;
    const bool utf8 = encoding_ == 65001;
    for (std::string& s : sections_) {
        if (!utf8) s = lib::to_utf8(s);
        // Links to offsets become links to the inserted anchors; the section is
        // found when the link is followed.
        s = lib::replace_all(s, "filepos=\"", "href=\"#filepos");
        s = lib::replace_all(s, "filepos='", "href='#filepos");
        for (size_t at = 0; (at = s.find("filepos=", at)) != std::string::npos;) {
            size_t end = at + 8;
            while (end < s.size() && isdigit((unsigned char)s[end])) ++end;
            s.replace(at, end - at, "href=\"#filepos" + s.substr(at + 8, end - at - 8) + "\"");
        }
        s = lib::replace_all(s, "<mbp:pagebreak", "<hr class=\"pagebreak\"");
    }
}

// KF8: the text is skeletons with fragments to insert; each skeleton plus its
// fragments is one HTML file (KindleUnpack's buildParts).
void Mobi::build_kf8(const std::string& text) {
    auto rec = [this](uint32_t i) {
        std::string r;
        record(i, r);
        return r;
    };
    // Flow 0 is the HTML; other flows are style sheets and SVG.
    std::string html = text;
    std::string fdst;
    if (fdst_ != kNone && record(fdst_, fdst) && fdst.compare(0, 4, "FDST") == 0) {
        const uint32_t n = be32(fdst, 8);
        if (n <= (fdst.size() >= 12 ? (fdst.size() - 12) / 8 : 0)) {
            flow_images_.resize(n);
            for (uint32_t i = 1; i < n; ++i) {
                const uint32_t a = be32(fdst, 12 + i * 8), b = be32(fdst, 16 + i * 8);
                if (a > b || b > text.size()) continue;
                const std::string flow = text.substr(a, b - a);
                if (flow.find("<svg") == std::string::npos) continue;
                auto root = lib::parse_markup(flow, lib::MarkupMode::Xml);
                if (const lib::Node* image = root->find("image")) flow_images_[i] = image->attr("href");
            }
        }
        if (n > 0) {
            const uint32_t start = be32(fdst, 12), end = be32(fdst, 16);
            if (start <= end && end <= text.size()) html = text.substr(start, end - start);
        }
    }
    for (const IndexEntry& e : read_index(rec, skeleton_index_, nullptr)) {
        auto f = e.tags.find(1);
        auto p = e.tags.find(6);
        if (f == e.tags.end() || p == e.tags.end() || f->second.empty() || p->second.size() < 2) continue;
        skeletons_.push_back({f->second[0], p->second[0], p->second[1]});
    }
    std::map<uint32_t, std::string> cncx;
    for (const IndexEntry& e : read_index(rec, fragment_index_, &cncx)) {
        auto file = e.tags.find(3);
        auto p = e.tags.find(6);
        if (p == e.tags.end() || p->second.size() < 2) continue;
        fragments_.push_back({(uint32_t)strtoul(e.name.c_str(), nullptr, 10),
                              file != e.tags.end() && !file->second.empty() ? file->second[0] : 0, p->second[0],
                              p->second[1]});
    }
    if (skeletons_.empty()) {
        // No indexes: the whole flow as one file.
        sections_.push_back(html);
        part_start_.push_back(0);
    }
    size_t frag = 0;
    for (const Skeleton& sk : skeletons_) {
        if ((uint64_t)sk.start + sk.length > html.size()) break;
        std::string part = html.substr(sk.start, sk.length);
        size_t base = sk.start + sk.length;
        for (uint32_t j = 0; j < sk.fragments && frag < fragments_.size(); ++j, ++frag) {
            const Fragment& f = fragments_[frag];
            const size_t insert = f.insert >= sk.start ? f.insert - sk.start : 0;
            if (base + f.length > html.size()) break;
            const std::string slice = html.substr(base, f.length);
            part.insert(std::min(insert, part.size()), slice);
            base += f.length;
        }
        part_start_.push_back(sk.start);
        sections_.push_back(std::move(part));
    }
    if (encoding_ != 65001)
        for (std::string& s : sections_) s = lib::to_utf8(s);
}

// kindle:pos:fid:XXXX:off:YYYYYYYYYY -> "mobi:<part>#<id of the element there>".
std::string Mobi::resolve_kindle_pos(const std::string& href) const {
    const size_t fid = href.find("fid:"), off = href.find(":off:");
    if (fid == std::string::npos || off == std::string::npos) return std::string();
    const uint32_t row = base32(href.substr(fid + 4, off - fid - 4));
    const uint32_t offset = base32(href.substr(off + 5));
    if (row >= fragments_.size() || sections_.empty() || part_start_.empty()) return std::string();
    const uint32_t pos = fragments_[row].insert + offset;
    // The part containing that position (parts are contiguous in the text).
    int part = 0;
    for (size_t i = 0; i < part_start_.size(); ++i)
        if (part_start_[i] <= pos) part = (int)i;
    const std::string& s = sections_[part];
    size_t at = std::min<size_t>(pos - part_start_[part], s.size());
    // The element starting at (or enclosing) that offset: its id, or Kindle's aid.
    for (size_t lt = s.rfind('<', at); lt != std::string::npos; lt = lt ? s.rfind('<', lt - 1) : std::string::npos) {
        const size_t gt = s.find('>', lt);
        const std::string tag = s.substr(lt, gt == std::string::npos ? 0 : gt - lt);
        for (const char* attr : {" id=\"", " aid=\""}) {
            const size_t a = tag.find(attr);
            if (a != std::string::npos) {
                const size_t start = a + strlen(attr);
                return "mobi:" + std::to_string(part) + "#" + tag.substr(start, tag.find('"', start) - start);
            }
        }
        if (lt == 0 || at - lt > 4096) break;
    }
    return "mobi:" + std::to_string(part);
}

// The NCX index: the table of contents.
void Mobi::read_ncx() {
    if (ncx_index_ == kNone) return;
    auto rec = [this](uint32_t i) {
        std::string r;
        record(i, r);
        return r;
    };
    std::map<uint32_t, std::string> cncx;
    for (const IndexEntry& e : read_index(rec, ncx_index_, &cncx)) {
        MobiTocEntry t;
        auto label = e.tags.find(3);
        if (label == e.tags.end() || label->second.empty()) continue;
        t.title = lib::to_utf8(cncx[label->second[0]]);
        auto depth = e.tags.find(4);
        t.level = depth != e.tags.end() && !depth->second.empty() ? (int)depth->second[0] + 1 : 1;
        auto posfid = e.tags.find(6);
        auto pos = e.tags.find(1);
        if (kf8_ && posfid != e.tags.end() && posfid->second.size() >= 2) {
            char buf[64];
            // Back into the kindle:pos form, so one resolver handles both.
            auto b32 = [](uint32_t v, int width) {
                static const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
                std::string s(width, '0');
                for (int i = width - 1; i >= 0; --i, v >>= 5) s[i] = digits[v & 31];
                return s;
            };
            snprintf(buf, sizeof buf, "kindle:pos:fid:%s:off:%s", b32(posfid->second[0], 4).c_str(),
                     b32(posfid->second[1], 10).c_str());
            const std::string target = resolve_kindle_pos(buf);
            const size_t hash = target.find('#');
            if (!lib::starts_with(target, "mobi:")) continue;
            t.section = atoi(target.c_str() + 5);
            t.anchor = hash == std::string::npos ? "" : target.substr(hash + 1);
        } else if (pos != e.tags.end() && !pos->second.empty()) {
            const uint32_t p = pos->second[0];
            int s = 0;
            for (size_t i = 0; i < section_start_.size(); ++i)
                if (section_start_[i] <= p) s = (int)i;
            t.section = s;
            t.anchor = "filepos" + std::to_string(p);
        } else {
            continue;
        }
        if (!t.title.empty()) toc_.push_back(std::move(t));
    }
}

bool Mobi::load_section(int index, Document& out, std::string& error) const {
    if (index < 0 || index >= (int)sections_.size()) {
        error = "No such section.";
        return false;
    }
    ConvertOptions options;
    const Mobi* self = this;
    options.resolve = [self](const std::string& href) -> std::string {
        if (lib::starts_with(href, "kindle:pos:")) return self->resolve_kindle_pos(href);
        std::string image = href;
        if (lib::starts_with(image, "kindle:flow:")) {
            const uint32_t flow = base32(image.substr(12));
            if (flow >= self->flow_images_.size()) return std::string();
            image = self->flow_images_[flow];
        }
        if (lib::starts_with(image, "kindle:embed:")) {
            const size_t end = image.find_first_of("?#", 13);
            const uint32_t n = base32(image.substr(13, end == std::string::npos ? std::string::npos : end - 13));
            return n ? self->image_key(self->first_resource_ + n - 1) : std::string();
        }
        if (lib::starts_with(href, "#filepos")) {
            // Which MOBI 6 section holds this offset.
            const uint32_t p = (uint32_t)strtoul(href.c_str() + 8, nullptr, 10);
            int s = 0;
            for (size_t i = 0; i < self->section_start_.size(); ++i)
                if (self->section_start_[i] <= p) s = (int)i;
            return "mobi:" + std::to_string(s) + "#filepos" + std::to_string(p);
        }
        if (lib::starts_with(href, "recindex:")) {
            const uint32_t n = (uint32_t)strtoul(href.c_str() + 9, nullptr, 10);
            return n ? self->image_key(self->first_resource_ + n - 1) : std::string();
        }
        return href;
    };
    options.min_image = 8;
    std::string html = sections_[index];
    // MOBI 6 pictures: <img recindex="00005"> -> a source the resolver understands.
    if (!kf8_) {
        for (size_t at = 0; (at = html.find("recindex=", at)) != std::string::npos;) {
            size_t p = at + 9;
            const bool quoted = p < html.size() && (html[p] == '"' || html[p] == '\'');
            if (quoted) ++p;
            size_t end = p;
            while (end < html.size() && isdigit((unsigned char)html[end])) ++end;
            const std::string number = html.substr(p, end - p);
            if (quoted && end < html.size()) ++end;
            // "hirecindex"/"lorecindex" variants keep their prefix and are ignored.
            const std::string replacement = "src=\"recindex:" + number + "\"";
            html.replace(at, end - at, replacement);
            at += replacement.size();
        }
    }
    html_to_document(*lib::parse_markup(html, lib::MarkupMode::Html), options, out);
    if (out.blocks.empty()) {
        Block b;
        b.runs.push_back({" ", 0, -1});
        out.blocks.push_back(b);
    }
    return true;
}

void register_mobi_images() {
    gfx::image_register_prefix_loader("mobi:", [](const std::string& source, std::string& bytes) {
        const size_t sep = source.find('\x01');
        if (sep == std::string::npos) return false;
        const std::string path = source.substr(5, sep - 5);
        const uint32_t index = (uint32_t)strtoul(source.c_str() + sep + 1, nullptr, 10);
        std::shared_ptr<const std::string> data;
        {
            std::lock_guard<std::mutex> lock(g_open_lock);
            auto found = g_open_files.find(path);
            if (found != g_open_files.end()) data = found->second.lock();
        }
        Mobi book;
        std::string error;
        if (!data) {
            if (!book.open(path, error)) return false;
            return book.record(index, bytes);
        }
        // Record offsets from the Palm database header.
        const std::string& d = *data;
        const uint16_t count = be16(d, 76);
        if (index >= count) return false;
        const uint32_t start = be32(d, 78 + index * 8);
        const uint32_t end = index + 1 < count ? be32(d, 78 + (index + 1) * 8) : (uint32_t)d.size();
        if (start > end || end > d.size()) return false;
        bytes.assign(d, start, end - start);
        return true;
    });
}

}  // namespace doc
