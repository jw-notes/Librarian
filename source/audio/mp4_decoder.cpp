// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// AAC in MP4 (M4B, M4A, MP4): Librarian's own demuxer over the sample tables,
// so seeking is exact and nothing but the tables is read ahead. Also raw AAC
// (ADTS) files.
#include <string.h>

#include <algorithm>
#include <functional>

#include "audio/aac.h"
#include "audio/decoder.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr uint32_t fourcc(const char* s) {
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3];
}

// Reads a descriptor length (1..4 bytes of 7 bits).
uint32_t descriptor_length(Reader& r) {
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t b = r.u8();
        n = n << 7 | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return n;
}

struct Track {
    uint32_t timescale = 0;
    uint64_t duration = 0;
    bool sound = false;
    uint8_t object_type = 0;
    std::vector<uint8_t> config;
    std::vector<std::pair<uint32_t, uint32_t>> stts;  // sample count, delta
    struct Stsc {
        uint32_t first_chunk, per_chunk;  // first_chunk counts from 1
    };
    std::vector<Stsc> stsc;
    uint32_t samples = 0, constant_size = 0;
    // A 20-hour book has about 3 million samples: sizes are kept in 16 bits
    // when they fit (AAC frames always do), about 6 MB.
    std::vector<uint16_t> sizes16;
    std::vector<uint32_t> sizes32;
    std::vector<uint64_t> chunks;

    uint32_t size_of(uint32_t s) const {
        if (constant_size) return constant_size;
        return sizes32.empty() ? sizes16[s] : sizes32[s];
    }
};

class Mp4Decoder : public Decoder {
public:
    explicit Mp4Decoder(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}
    const char* name() const override { return "mp4"; }
    void cancel() override { source_->cancel(); }

    bool open(std::string& error) override {
        Reader r(*source_);
        if (!boxes(r, r.size(), 0)) {
            error = source_->error().empty() ? "The MP4 file is damaged." : source_->error();
            return false;
        }
        if (!found_) {
            error = "The file has no audio track.";
            return false;
        }
        if (t_.object_type != 0x40 && t_.object_type != 0x66 && t_.object_type != 0x67 && t_.object_type != 0x68) {
            error = "The audio in this MP4 file is not AAC (object type " + std::to_string(t_.object_type) + ").";
            return false;
        }
        if (t_.config.empty() || !t_.timescale || t_.chunks.empty() || t_.stsc.empty() || !t_.samples ||
            (!t_.constant_size && t_.sizes16.size() + t_.sizes32.size() < t_.samples)) {
            error = "The MP4 file's sample tables are missing or damaged.";
            return false;
        }
        if (!faad_.init_config(t_.config.data(), t_.config.size(), rate_, channels_)) {
            error = faad_.last_error;
            return false;
        }
        duration_ = (double)t_.duration / t_.timescale;
        if (duration_ <= 0) {
            uint64_t total = 0;
            for (const auto& e : t_.stts) total += (uint64_t)e.first * e.second;
            duration_ = (double)total / t_.timescale;
        }
        locate(0);
        return true;
    }

    bool seek(double seconds) override {
        const uint64_t target = (uint64_t)(std::max(0.0, seconds) * t_.timescale);
        uint64_t t = 0;
        uint32_t s = 0;
        for (const auto& [count, delta] : t_.stts) {
            if (delta && t + (uint64_t)count * delta > target) {
                s += (uint32_t)((target - t) / delta);
                break;
            }
            t += (uint64_t)count * delta;
            s += count;
        }
        // Two frames early: AAC frames overlap, so the first one after a jump
        // is not complete. The player skips to the exact time.
        s = s >= 2 ? s - 2 : 0;
        locate(std::min(s, t_.samples));
        faad_.reset((long)sample_);
        return true;
    }

    int decode(std::vector<float>& out) override {
        while (sample_ < t_.samples) {
            const uint32_t size = t_.size_of(sample_);
            frame_.resize(size);
            if (source_->read_at(offset_, frame_.data(), size) != size) {
                error_ = source_->error().empty() ? "The audio file ended early." : source_->error();
                return -1;
            }
            const uint32_t delta = advance();
            int rate = rate_, channels = channels_;
            const int n = faad_.decode(frame_.data(), size, out, rate, channels);
            position_ = (double)time_ / t_.timescale;
            if (n < 0) {
                // One damaged frame is skipped; many in a row end playback.
                if (++bad_ > 50) {
                    error_ = "The AAC audio could not be decoded: " + faad_.last_error;
                    return -1;
                }
                continue;
            }
            bad_ = 0;
            (void)delta;
            if (n == 0) continue;
            rate_ = rate;
            channels_ = channels;
            return n;
        }
        return 0;
    }

private:
    // Puts the read position on sample `s`.
    void locate(uint32_t s) {
        sample_ = s;
        time_ = 0;
        uint32_t counted = 0;
        stts_i_ = 0;
        stts_left_ = 0;
        for (size_t i = 0; i < t_.stts.size(); ++i) {
            const auto& [count, delta] = t_.stts[i];
            if (counted + count > s) {
                time_ += (uint64_t)(s - counted) * delta;
                stts_i_ = i;
                stts_left_ = count - (s - counted);
                break;
            }
            time_ += (uint64_t)count * delta;
            counted += count;
            stts_i_ = i + 1;
        }
        position_ = (double)time_ / t_.timescale;
        // The chunk holding sample s.
        uint32_t first_sample = 0;
        for (size_t i = 0; i < t_.stsc.size(); ++i) {
            const uint32_t from = t_.stsc[i].first_chunk - 1;
            const uint32_t to = i + 1 < t_.stsc.size() ? t_.stsc[i + 1].first_chunk - 1 : (uint32_t)t_.chunks.size();
            const uint32_t per = std::max<uint32_t>(1, t_.stsc[i].per_chunk);
            const uint64_t span = (uint64_t)(to - from) * per;
            if (s < first_sample + span || i + 1 == t_.stsc.size()) {
                chunk_ = std::min<uint32_t>(from + (s - first_sample) / per, (uint32_t)t_.chunks.size() - 1);
                in_chunk_ = (s - first_sample) % per;
                stsc_i_ = i;
                break;
            }
            first_sample += (uint32_t)span;
        }
        offset_ = t_.chunks[chunk_];
        for (uint32_t k = s - in_chunk_; k < s; ++k) offset_ += t_.size_of(k);
    }

    // Moves past the current sample; returns its duration.
    uint32_t advance() {
        offset_ += t_.size_of(sample_);
        ++sample_;
        uint32_t delta = 0;
        while (stts_i_ < t_.stts.size() && stts_left_ == 0) {
            if (++stts_i_ < t_.stts.size()) stts_left_ = t_.stts[stts_i_].first;
        }
        if (stts_i_ < t_.stts.size()) {
            delta = t_.stts[stts_i_].second;
            --stts_left_;
        }
        time_ += delta;
        if (++in_chunk_ >= std::max<uint32_t>(1, t_.stsc[stsc_i_].per_chunk)) {
            in_chunk_ = 0;
            if (chunk_ + 1 < t_.chunks.size()) {
                ++chunk_;
                if (stsc_i_ + 1 < t_.stsc.size() && chunk_ + 1 >= t_.stsc[stsc_i_ + 1].first_chunk) ++stsc_i_;
                offset_ = t_.chunks[chunk_];
            }
        }
        return delta;
    }

    // ---- parsing ----------------------------------------------------------------------

    bool boxes(Reader& r, uint64_t end, int depth) {
        while (r.tell() + 8 <= end && !found_) {
            const uint64_t start = r.tell();
            uint64_t size = r.u32();
            const uint32_t type = r.u32();
            if (size == 1) size = r.u64();
            else if (size == 0) size = end - start;
            if (!r.ok() || size < 8 || start + size > end) return r.ok() && depth > 0;  // trailing junk is tolerated
            const uint64_t box_end = start + size;
            if (!box(r, type, box_end, depth)) return false;
            r.seek(box_end);
        }
        return r.ok();
    }

    bool box(Reader& r, uint32_t type, uint64_t end, int depth) {
        switch (type) {
            case fourcc("moov"):
            case fourcc("mdia"):
            case fourcc("minf"):
            case fourcc("stbl"):
                return boxes(r, end, depth + 1);
            case fourcc("trak"): {
                t_ = Track();
                if (!boxes(r, end, depth + 1)) return false;
                if (t_.sound && !t_.config.empty()) found_ = true;
                return true;
            }
            case fourcc("mdhd"): {
                const uint8_t version = r.u8();
                r.skip(3);
                if (version == 1) {
                    r.skip(16);
                    t_.timescale = r.u32();
                    t_.duration = r.u64();
                } else {
                    r.skip(8);
                    t_.timescale = r.u32();
                    t_.duration = r.u32();
                }
                return r.ok();
            }
            case fourcc("hdlr"):
                r.skip(8);
                t_.sound = r.u32() == fourcc("soun");
                return r.ok();
            case fourcc("stsd"): {
                r.skip(8);  // version, flags, entry count: the first entry is used
                const uint64_t entry = r.tell();
                const uint32_t entry_size = r.u32(), format = r.u32();
                if (format != fourcc("mp4a")) return true;
                r.skip(8);  // reserved, data reference
                const uint16_t version = r.u16();
                r.skip(18);  // revision, vendor, channels, bits, compression, packet size, rate
                if (version == 1) r.skip(16);
                else if (version == 2) r.skip(36);
                return mp4a_children(r, std::min<uint64_t>(entry + entry_size, end));
            }
            case fourcc("stts"): {
                r.skip(4);
                const uint32_t n = r.u32();
                if ((uint64_t)n * 8 > end - r.tell()) return false;
                t_.stts.resize(n);
                for (auto& e : t_.stts) {
                    e.first = r.u32();
                    e.second = r.u32();
                }
                return r.ok();
            }
            case fourcc("stsc"): {
                r.skip(4);
                const uint32_t n = r.u32();
                if ((uint64_t)n * 12 > end - r.tell()) return false;
                t_.stsc.resize(n);
                for (auto& e : t_.stsc) {
                    e.first_chunk = std::max<uint32_t>(1, r.u32());
                    e.per_chunk = r.u32();
                    r.skip(4);
                }
                return r.ok();
            }
            case fourcc("stsz"): {
                r.skip(4);
                t_.constant_size = r.u32();
                t_.samples = r.u32();
                if (t_.constant_size) return r.ok();
                if ((uint64_t)t_.samples * 4 > end - r.tell()) return false;
                // Read in slices: the table of a long book is megabytes.
                std::vector<uint8_t> slice(64 * 1024);
                t_.sizes16.reserve(t_.samples);
                for (uint32_t done = 0; done < t_.samples;) {
                    const uint32_t k = std::min<uint32_t>(t_.samples - done, (uint32_t)slice.size() / 4);
                    if (!r.read(slice.data(), (size_t)k * 4)) return false;
                    for (uint32_t i = 0; i < k; ++i) {
                        const uint8_t* p = slice.data() + i * 4;
                        const uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
                        if (v > 0xFFFF && t_.sizes32.empty()) t_.sizes32.assign(t_.sizes16.begin(), t_.sizes16.end());
                        if (!t_.sizes32.empty()) t_.sizes32.push_back(v);
                        else t_.sizes16.push_back((uint16_t)v);
                    }
                    done += k;
                }
                if (!t_.sizes32.empty()) std::vector<uint16_t>().swap(t_.sizes16);
                return true;
            }
            case fourcc("stco"):
            case fourcc("co64"): {
                r.skip(4);
                const uint32_t n = r.u32();
                const int width = type == fourcc("co64") ? 8 : 4;
                if ((uint64_t)n * width > end - r.tell()) return false;
                t_.chunks.resize(n);
                for (auto& c : t_.chunks) c = width == 8 ? r.u64() : r.u32();
                return r.ok();
            }
            default:
                return true;
        }
    }

    bool mp4a_children(Reader& r, uint64_t end) {
        while (r.tell() + 8 <= end) {
            const uint64_t start = r.tell();
            const uint32_t size = r.u32(), type = r.u32();
            if (size < 8 || start + size > end) return r.ok();
            if (type == fourcc("esds")) {
                r.skip(4);
                if (r.u8() != 0x03) return true;
                descriptor_length(r);
                r.skip(2);
                const uint8_t flags = r.u8();
                if (flags & 0x80) r.skip(2);
                if (flags & 0x40) r.skip(r.u8());
                if (flags & 0x20) r.skip(2);
                if (r.u8() != 0x04) return true;
                descriptor_length(r);
                t_.object_type = r.u8();
                r.skip(12);
                if (r.u8() == 0x05) {
                    const uint32_t n = descriptor_length(r);
                    if (n > 64) return false;
                    t_.config.resize(n);
                    if (!r.read(t_.config.data(), n)) return false;
                }
                return r.ok();
            }
            r.seek(start + size);
        }
        return r.ok();
    }

    std::shared_ptr<ByteSource> source_;
    Track t_;
    bool found_ = false;
    Faad faad_;
    std::vector<uint8_t> frame_;
    uint32_t sample_ = 0, chunk_ = 0, in_chunk_ = 0, stts_left_ = 0;
    size_t stsc_i_ = 0, stts_i_ = 0;
    uint64_t offset_ = 0, time_ = 0;
    int bad_ = 0;
};

// Raw AAC: a sequence of ADTS frames.
class AdtsDecoder : public Decoder {
public:
    explicit AdtsDecoder(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}
    const char* name() const override { return "aac"; }
    void cancel() override { source_->cancel(); }

    bool open(std::string& error) override {
        if (!fill(0) || !adts_sync(buf_.data(), buf_.size(), pos_) ||
            !faad_.init_adts(buf_.data() + pos_, buf_.size() - pos_, rate_, channels_)) {
            error = faad_.last_error.empty() ? "Not an AAC (ADTS) stream." : faad_.last_error;
            return false;
        }
        start_ = at_ + pos_;
        const size_t n = adts_frame_length(buf_.data() + pos_, buf_.size() - pos_);
        // Bitrate from the first frame, for a duration when the server gave none.
        if (n && rate_) duration_ = hint_ > 0 ? hint_ : (double)(source_->size() - start_) / n * 1024.0 / rate_;
        return true;
    }

    bool seek(double seconds) override {
        const double d = hint_ > 0 ? hint_ : duration_;
        const uint64_t span = source_->size() - start_;
        const uint64_t at = seconds <= 0 || d <= 0 ? start_ : start_ + (uint64_t)(std::min(1.0, seconds / d) * span);
        pos_ = 0;
        if (!fill(at)) return false;
        adts_sync(buf_.data(), buf_.size(), pos_);
        faad_.reset(0);
        position_ = std::max(0.0, seconds);
        return true;
    }

    int decode(std::vector<float>& out) override {
        for (;;) {
            if (buf_.size() - pos_ < 8192 && at_ + buf_.size() < source_->size()) fill(at_ + pos_);
            const size_t n = adts_frame_length(buf_.data() + pos_, buf_.size() - pos_);
            if (!n || pos_ + n > buf_.size()) {
                if (at_ + buf_.size() >= source_->size()) return 0;
                if (!adts_sync(buf_.data(), buf_.size(), ++pos_)) return 0;
                continue;
            }
            int rate = rate_, channels = channels_;
            const int frames = faad_.decode(buf_.data() + pos_, n, out, rate, channels);
            pos_ += n;
            if (frames < 0) continue;
            if (frames == 0) continue;
            rate_ = rate;
            channels_ = channels;
            position_ += (double)frames / rate_;
            return frames;
        }
    }

private:
    bool fill(uint64_t at) {
        buf_.resize(64 * 1024);
        const size_t got = source_->read_at(at, buf_.data(), buf_.size());
        buf_.resize(got);
        at_ = at;
        pos_ = 0;
        return got > 0;
    }

    std::shared_ptr<ByteSource> source_;
    Faad faad_;
    std::vector<uint8_t> buf_;
    uint64_t at_ = 0, start_ = 0;
    size_t pos_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> make_mp4_decoder(std::shared_ptr<ByteSource> source) { return std::make_unique<Mp4Decoder>(source); }
std::unique_ptr<Decoder> make_adts_decoder(std::shared_ptr<ByteSource> source) { return std::make_unique<AdtsDecoder>(source); }

}  // namespace audio
