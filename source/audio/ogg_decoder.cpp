// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Ogg audio: Opus (libopus) and Vorbis (stb_vorbis), read through a ByteSource
// (a file, Range requests, or a USB drive over FTP), so only the pages in use
// are ever read. The Ogg layer is Librarian's own: page headers with their
// CRC, the length from the last page's granule position, and seeking by
// bisection on granule positions. Ogg FLAC goes to dr_flac (flac_decoder.cpp).
#include <string.h>

#include <algorithm>
#include <cmath>

#include "audio/decoder.h"
#include "core/log.h"

#include "opus/include/opus_multistream.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#define STB_VORBIS_NO_PULLDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
// stb_vorbis.c leaves these macros defined; they clash with C++ headers.
#undef L
#undef C
#undef R

namespace audio {

std::unique_ptr<Decoder> make_flac_decoder(std::shared_ptr<ByteSource> source);

namespace ogg {

// ---- pages -------------------------------------------------------------------------------

uint32_t crc_table[256];
bool crc_ready = false;

void init_crc() {
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        crc_table[i] = r;
    }
    crc_ready = true;
}

struct Page {
    uint64_t offset = 0;      // where "OggS" starts
    uint32_t header = 0;      // header bytes (27 + segment table)
    uint32_t body = 0;        // body bytes
    int64_t granule = -1;     // -1: no packet ends on this page
    uint32_t serial = 0;
    uint8_t flags = 0;        // 1 continued, 2 first, 4 last
    std::vector<uint8_t> lacing;
    uint64_t end() const { return offset + header + body; }
    bool continued() const { return flags & 1; }
};

// Reads and checks the page at `offset` (capture pattern, version, CRC).
// `body`, if given, receives the page body.
bool read_page(ByteSource& src, uint64_t offset, Page& p, std::string* body = nullptr) {
    uint8_t h[27];
    if (src.read_at(offset, h, 27) != 27) return false;
    if (memcmp(h, "OggS", 4) || h[4] != 0) return false;
    p.offset = offset;
    p.flags = h[5];
    uint64_t g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | h[6 + i];
    p.granule = (int64_t)g;
    p.serial = h[14] | (h[15] << 8) | (h[16] << 16) | ((uint32_t)h[17] << 24);
    const uint32_t crc = h[22] | (h[23] << 8) | (h[24] << 16) | ((uint32_t)h[25] << 24);
    const int nseg = h[26];
    p.lacing.resize(nseg);
    if (nseg && src.read_at(offset + 27, p.lacing.data(), nseg) != (size_t)nseg) return false;
    p.header = 27 + nseg;
    p.body = 0;
    for (uint8_t l : p.lacing) p.body += l;
    std::string local;
    std::string& data = body ? *body : local;
    data.resize(p.body);
    if (p.body && src.read_at(offset + p.header, &data[0], p.body) != p.body) return false;
    // The CRC covers the header (its CRC field as zeros) and the body.
    init_crc();
    uint32_t c = 0;
    auto add = [&](const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n; ++i) c = (c << 8) ^ crc_table[((c >> 24) ^ d[i]) & 0xFF];
    };
    uint8_t hz[27];
    memcpy(hz, h, 27);
    memset(hz + 22, 0, 4);
    add(hz, 27);
    add(p.lacing.data(), p.lacing.size());
    add((const uint8_t*)data.data(), data.size());
    return c == crc;
}

// The first valid page of stream `serial` starting at or after `from` (and
// before `limit`). Scans in blocks for the capture pattern.
bool find_page(ByteSource& src, uint64_t from, uint64_t limit, uint32_t serial, Page& out) {
    limit = std::min<uint64_t>(limit, src.size());
    std::string block;
    while (from + 27 <= limit) {
        if (src.cancelled()) return false;
        const size_t n = (size_t)std::min<uint64_t>(65536, limit - from);
        block.resize(n);
        const size_t got = src.read_at(from, &block[0], n);
        if (got < 4) return false;
        for (size_t i = 0; i + 4 <= got; ++i) {
            if (memcmp(block.data() + i, "OggS", 4)) continue;
            Page p;
            if (read_page(src, from + i, p) && p.serial == serial) {
                out = std::move(p);
                return true;
            }
        }
        from += got - 3;  // a pattern may straddle the blocks
    }
    return false;
}

// The granule position of the last page of stream `serial` (-1 if none).
int64_t last_granule(ByteSource& src, uint32_t serial) {
    const uint64_t size = src.size();
    uint64_t chunk = 65536;
    while (true) {
        const uint64_t from = size > chunk ? size - chunk : 0;
        int64_t best = -1;
        uint64_t at = from;
        Page p;
        while (find_page(src, at, size, serial, p)) {
            if (p.granule != -1) best = p.granule;
            at = p.end();
        }
        if (best != -1 || from == 0) return best;
        chunk *= 4;  // the stream's end page may be far back (a large final page)
        if (chunk > (1u << 24)) return -1;
    }
}

// Packets of one logical stream, in order, from a starting page.
class Packets {
public:
    Packets(ByteSource& src, uint32_t serial) : src_(&src), serial_(serial) {}

    // Starts at the page at `offset`. Packets that end on that page are
    // skipped when `skip_first_page` (their start lies on earlier pages); a
    // packet begun at its end is kept, so the first packet returned starts
    // exactly at that page's granule position.
    void start(uint64_t offset, bool skip_first_page) {
        next_ = offset;
        seg_ = 0;
        partial_.clear();
        page_ = Page();
        have_page_ = false;
        skip_page_ = skip_first_page;
        drop_continuation_ = true;
    }

    // The next complete packet; false at the end of the stream (or an error).
    bool next(std::string& packet) {
        while (true) {
            if (!have_page_) {
                if (!find_page(*src_, next_, src_->size(), serial_, page_)) return false;
                read_page(*src_, page_.offset, page_, &body_);
                next_ = page_.end();
                have_page_ = true;
                seg_ = 0;
                body_pos_ = 0;
                if (!page_.continued()) partial_.clear();
                if (drop_continuation_ && page_.continued()) skipping_partial_ = true;
                drop_continuation_ = false;
            }
            while (seg_ < page_.lacing.size()) {
                const uint8_t l = page_.lacing[seg_++];
                if (!skipping_partial_) partial_.append(body_, body_pos_, l);
                body_pos_ += l;
                if (l < 255) {
                    const bool skip = skipping_partial_ || skip_page_;
                    skipping_partial_ = false;
                    if (skip) {
                        partial_.clear();
                        continue;
                    }
                    packet.swap(partial_);
                    partial_.clear();
                    return true;
                }
            }
            // A packet left open at the end of the skipped page starts at its granule.
            skip_page_ = false;
            have_page_ = false;
        }
    }

private:
    ByteSource* src_;
    uint32_t serial_;
    uint64_t next_ = 0;
    Page page_;
    std::string body_, partial_;
    size_t seg_ = 0, body_pos_ = 0;
    bool have_page_ = false, skip_page_ = false, drop_continuation_ = true, skipping_partial_ = false;
};

// The last page of `serial` at or before `target` granule, searched between
// `lo` (a page known to be before the target) and the end. Its offset and
// granule; false if the target is before every page.
bool page_before(ByteSource& src, uint32_t serial, uint64_t lo, int64_t target, Page& out) {
    uint64_t hi = src.size();
    Page p;
    // Bisection down to a window of a few pages.
    while (hi > lo && hi - lo > 131072) {
        if (src.cancelled()) return false;
        const uint64_t mid = lo + (hi - lo) / 2;
        Page m;
        bool found = false;
        uint64_t at = mid;
        // Skip pages where no packet ends (granule -1).
        while (find_page(src, at, hi, serial, m)) {
            if (m.granule != -1) {
                found = true;
                break;
            }
            at = m.end();
        }
        if (!found || m.granule >= target) hi = mid;
        else lo = m.offset;
    }
    bool any = false;
    uint64_t at = lo;
    while (find_page(src, at, src.size(), serial, p)) {
        if (p.granule != -1) {
            if (p.granule >= target) break;
            out = p;
            any = true;
        }
        at = p.end();
    }
    return any;
}

// The first page of the file: its serial and first packet (the codec's ID header).
bool first_packet(ByteSource& src, uint32_t& serial, std::string& packet) {
    Page p;
    std::string body;
    if (!read_page(src, 0, p, &body) || !(p.flags & 2)) return false;
    serial = p.serial;
    size_t len = 0;
    for (uint8_t l : p.lacing) {
        len += l;
        if (l < 255) break;
    }
    packet = body.substr(0, len);
    return true;
}

}  // namespace ogg

namespace {

// ---- Opus --------------------------------------------------------------------------------

class OpusDecoder : public Decoder {
public:
    explicit OpusDecoder(std::shared_ptr<ByteSource> source) : source_(std::move(source)), packets_(*source_, 0) {}
    ~OpusDecoder() override {
        if (dec_) opus_multistream_decoder_destroy(dec_);
    }
    const char* name() const override { return "opus"; }
    void cancel() override { source_->cancel(); }

    bool open(std::string& error) override {
        std::string head;
        if (!ogg::first_packet(*source_, serial_, head) || head.size() < 19 || head.compare(0, 8, "OpusHead")) {
            error = source_->error().empty() ? "Not an Opus file, or it is damaged." : source_->error();
            return false;
        }
        const uint8_t* h = (const uint8_t*)head.data();
        channels_ = h[9];
        pre_skip_ = h[10] | (h[11] << 8);
        const int16_t gain = (int16_t)(h[16] | (h[17] << 8));
        const int family = h[18];
        int streams = 1, coupled = channels_ > 1 ? 1 : 0;
        unsigned char mapping[255] = {0, 1};
        if (family != 0) {
            if (head.size() < (size_t)21 + channels_) {
                error = "The Opus file's channel table is damaged.";
                return false;
            }
            streams = h[19];
            coupled = h[20];
            memcpy(mapping, h + 21, channels_);
        } else if (channels_ > 2) {
            error = "The Opus file's header is damaged.";
            return false;
        }
        if (channels_ < 1) {
            error = "The Opus file has no audio.";
            return false;
        }
        int err = 0;
        dec_ = opus_multistream_decoder_create(48000, channels_, streams, coupled, mapping, &err);
        if (!dec_ || err != OPUS_OK) {
            error = std::string("Opus: ") + opus_strerror(err);
            return false;
        }
        if (gain) opus_multistream_decoder_ctl(dec_, OPUS_SET_GAIN(gain));
        rate_ = 48000;
        packets_ = ogg::Packets(*source_, serial_);
        // Past the two header packets: where the audio starts.
        packets_.start(0, false);
        std::string skip;
        if (!packets_.next(skip) || !packets_.next(skip) || skip.compare(0, 8, "OpusTags")) {
            error = "The Opus file's headers are damaged.";
            return false;
        }
        end_granule_ = ogg::last_granule(*source_, serial_);
        duration_ = end_granule_ > pre_skip_ ? (double)(end_granule_ - pre_skip_) / 48000.0 : hint_;
        gpos_ = 0;
        want_ = pre_skip_;
        position_ = 0;
        return true;
    }

    bool seek(double seconds) override {
        const int64_t target = (int64_t)(std::max(0.0, seconds) * 48000.0) + pre_skip_;
        // 80 ms of decoding before the target lets the decoder settle (RFC 7845).
        const int64_t from = target - 3840;
        ogg::Page p;
        opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE);
        if (from > pre_skip_ && ogg::page_before(*source_, serial_, 0, from, p)) {
            packets_.start(p.offset, true);
            gpos_ = p.granule;
        } else {
            // Near the start: from the first audio packet, after the headers.
            packets_.start(0, false);
            std::string skip;
            packets_.next(skip);
            packets_.next(skip);
            gpos_ = 0;
        }
        if (source_->cancelled()) return false;
        want_ = std::max<int64_t>(target, pre_skip_);
        position_ = (double)(std::max(gpos_, want_) - pre_skip_) / 48000.0;
        return true;
    }

    int decode(std::vector<float>& out) override {
        std::string packet;
        while (true) {
            if (end_granule_ > 0 && gpos_ >= end_granule_) return 0;
            if (!packets_.next(packet)) {
                if (!source_->error().empty()) {
                    error_ = source_->error();
                    return -1;
                }
                return 0;
            }
            pcm_.resize((size_t)5760 * channels_);
            const int n = opus_multistream_decode_float(dec_, (const unsigned char*)packet.data(), (int)packet.size(),
                                                        pcm_.data(), 5760, 0);
            if (n < 0) {
                // A damaged packet: skip it, as players do.
                LOG("audio: opus packet not decoded (%s)", opus_strerror(n));
                continue;
            }
            int64_t first = gpos_, count = n;
            gpos_ += n;
            // Before the wanted position (pre-skip, seek pre-roll): dropped.
            if (gpos_ <= want_) continue;
            int64_t skip = std::max<int64_t>(0, want_ - first);
            // Past the end the stream declares (encoder padding): cut.
            if (end_granule_ > 0 && gpos_ > end_granule_) count -= gpos_ - end_granule_;
            if (count <= skip) continue;
            const size_t at = out.size();
            out.insert(out.end(), pcm_.begin() + skip * channels_, pcm_.begin() + count * channels_);
            const int frames = (int)(count - skip);
            position_ = (double)(first + count - pre_skip_) / 48000.0;
            (void)at;
            return frames;
        }
    }

private:
    std::shared_ptr<ByteSource> source_;
    ogg::Packets packets_;
    OpusMSDecoder* dec_ = nullptr;
    uint32_t serial_ = 0;
    int pre_skip_ = 0;
    int64_t end_granule_ = -1, gpos_ = 0, want_ = 0;
    std::vector<float> pcm_;
};

// ---- Vorbis ------------------------------------------------------------------------------

class VorbisDecoder : public Decoder {
public:
    explicit VorbisDecoder(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}
    ~VorbisDecoder() override {
        if (v_) stb_vorbis_close(v_);
    }
    const char* name() const override { return "vorbis"; }
    void cancel() override { source_->cancel(); }

    bool open(std::string& error) override {
        std::string id;
        if (!ogg::first_packet(*source_, serial_, id)) {
            error = source_->error().empty() ? "Not an Ogg Vorbis file, or it is damaged." : source_->error();
            return false;
        }
        // The headers (identification, comments with any embedded cover,
        // codebooks) may be large: offer more until they fit.
        size_t want = 65536;
        while (true) {
            buf_.resize((size_t)std::min<uint64_t>(want, source_->size()));
            const size_t got = source_->read_at(0, buf_.data(), buf_.size());
            buf_.resize(got);
            int used = 0, err = 0;
            v_ = stb_vorbis_open_pushdata(buf_.data(), (int)buf_.size(), &used, &err, nullptr);
            if (v_) {
                pos_ = used;
                feed_ = got;
                break;
            }
            if (err != VORBIS_need_more_data || got < want || want >= (16u << 20)) {
                error = source_->error().empty() ? "Not an Ogg Vorbis file, or it is damaged." : source_->error();
                return false;
            }
            want *= 4;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(v_);
        rate_ = (int)info.sample_rate;
        channels_ = info.channels;
        if (rate_ <= 0 || channels_ <= 0) {
            error = "The Ogg Vorbis file has no audio.";
            return false;
        }
        const int64_t last = ogg::last_granule(*source_, serial_);
        duration_ = last > 0 ? (double)last / rate_ : hint_;
        end_ = last;
        sample_ = 0;
        want_ = 0;
        position_ = 0;
        return true;
    }

    bool seek(double seconds) override {
        const int64_t target = (int64_t)(std::max(0.0, seconds) * rate_);
        ogg::Page p;
        stb_vorbis_flush_pushdata(v_);
        buf_.clear();
        pos_ = 0;
        if (target > 0 && ogg::page_before(*source_, serial_, 0, target, p)) {
            feed_ = p.offset;
            sample_ = p.granule;  // corrected from the decoder once it knows
            exact_ = false;
        } else {
            // From the first audio page: the decoder skips the header pages itself.
            feed_ = 0;
            sample_ = 0;
            exact_ = false;
        }
        if (source_->cancelled()) return false;
        want_ = target;
        position_ = (double)std::max(sample_, want_) / rate_;
        return true;
    }

    int decode(std::vector<float>& out) override {
        while (true) {
            int ch = 0, n = 0;
            float** pcm = nullptr;
            const int used = stb_vorbis_decode_frame_pushdata(v_, buf_.data() + pos_, (int)(buf_.size() - pos_), &ch,
                                                              &pcm, &n);
            if (used == 0 && n == 0) {
                if (!refill()) {
                    if (!source_->error().empty()) {
                        error_ = source_->error();
                        return -1;
                    }
                    return 0;
                }
                continue;
            }
            pos_ += used;
            if (n <= 0) continue;
            if (!exact_) {
                // After a seek: where this frame ends, from the decoder.
                const int next = stb_vorbis_get_sample_offset(v_);
                if (next >= n) sample_ = next - n;
                exact_ = next >= 0;
            }
            int64_t first = sample_, count = n;
            sample_ += n;
            if (sample_ <= want_) continue;
            const int64_t skip = std::max<int64_t>(0, want_ - first);
            if (end_ > 0 && sample_ > end_) count -= sample_ - end_;
            if (count <= skip) continue;
            const size_t at = out.size();
            out.resize(at + (size_t)(count - skip) * channels_);
            float* o = out.data() + at;
            for (int64_t i = skip; i < count; ++i)
                for (int c = 0; c < channels_; ++c) *o++ = pcm[std::min(c, ch - 1)][i];
            position_ = (double)(first + count) / rate_;
            return (int)(count - skip);
        }
    }

private:
    bool refill() {
        if (pos_ > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + pos_);
            pos_ = 0;
        }
        if (feed_ >= source_->size() || source_->cancelled()) return false;
        const size_t n = (size_t)std::min<uint64_t>(32768, source_->size() - feed_);
        const size_t at = buf_.size();
        buf_.resize(at + n);
        const size_t got = source_->read_at(feed_, buf_.data() + at, n);
        buf_.resize(at + got);
        feed_ += got;
        return got > 0;
    }

    std::shared_ptr<ByteSource> source_;
    stb_vorbis* v_ = nullptr;
    uint32_t serial_ = 0;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    uint64_t feed_ = 0;
    int64_t sample_ = 0, want_ = 0, end_ = -1;
    bool exact_ = true;
};

}  // namespace

// The codec inside an Ogg file, from its first packet.
std::unique_ptr<Decoder> make_ogg_decoder(std::shared_ptr<ByteSource> source) {
    uint32_t serial = 0;
    std::string id;
    if (!ogg::first_packet(*source, serial, id)) return nullptr;
    if (id.compare(0, 8, "OpusHead") == 0) return std::make_unique<OpusDecoder>(source);
    if (id.size() >= 7 && id[0] == 1 && id.compare(1, 6, "vorbis") == 0) return std::make_unique<VorbisDecoder>(source);
    if (id.size() >= 30 && id[0] == 0x7F && id.compare(1, 4, "FLAC") == 0) {
        // dr_flac does not know an Ogg FLAC stream's length: the last page does
        // (STREAMINFO's sample rate follows "fLaC" and the block header).
        auto d = make_flac_decoder(source);
        const uint8_t* si = (const uint8_t*)id.data() + 17;
        const int rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
        const int64_t last = ogg::last_granule(*source, serial);
        if (rate > 0 && last > 0) d->set_duration_hint((double)last / rate);
        return d;
    }
    LOG("audio: an Ogg stream of another kind (Speex, Theora...) is not supported");
    return nullptr;
}

}  // namespace audio
