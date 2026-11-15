// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// MP3 through minimp3. Seeking uses the Xing/Info table of contents when the
// file has one (VBR), and otherwise the byte position for the time (exact for
// constant-bitrate files, as audiobooks usually are).
#include <string.h>

#include <algorithm>
#include <cmath>

#include "audio/decoder.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3/minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace audio {

namespace {

class Mp3Decoder : public Decoder {
public:
    explicit Mp3Decoder(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}
    const char* name() const override { return "mp3"; }
    void cancel() override { source_->cancel(); }

    bool open(std::string& error) override {
        end_ = source_->size();
        // ID3v2 tags (there may be several) at the start, ID3v1 at the end.
        uint8_t h[10];
        while (source_->read_at(start_, h, 10) == 10 && !memcmp(h, "ID3", 3)) {
            const uint64_t size = (uint64_t)(h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F);
            start_ += 10 + size + ((h[5] & 0x10) ? 10 : 0);
        }
        if (end_ > 128 && source_->read_at(end_ - 128, h, 3) == 3 && !memcmp(h, "TAG", 3)) end_ -= 128;
        mp3dec_init(&dec_);
        at_ = start_;
        fill();
        // The first frame: its format, and a Xing/Info header in it.
        mp3dec_frame_info_t info{};
        while (pos_ < buf_.size()) {
            const int samples = mp3dec_decode_frame(&dec_, buf_.data() + pos_, (int)(buf_.size() - pos_), pcm_, &info);
            if (!info.frame_bytes) break;
            if (samples > 0 || info.hz) {
                rate_ = info.hz;
                channels_ = info.channels;
                bitrate_ = info.bitrate_kbps;
                const uint64_t frame_at = at_ + pos_ + info.frame_offset;
                read_xing(frame_at, info);
                // A Xing frame holds no sound: playing starts after it.
                // (frame_bytes counts any skipped bytes before the frame too.)
                start_ = xing_frames_ ? at_ + pos_ + info.frame_bytes : frame_at;
                break;
            }
            pos_ += info.frame_bytes;
        }
        if (!rate_ || !channels_) {
            error = source_->error().empty() ? "No MPEG audio frames were found." : source_->error();
            return false;
        }
        const int per_frame = info.layer == 3 && rate_ < 32000 ? 576 : 1152;
        if (xing_frames_) duration_ = (double)xing_frames_ * per_frame / rate_;
        else if (hint_ > 0) duration_ = hint_;
        else if (bitrate_) duration_ = (double)(end_ - start_) * 8 / (bitrate_ * 1000.0);
        return seek(0);
    }

    bool seek(double seconds) override {
        seconds = std::max(0.0, seconds);
        const double d = duration_ > 0 ? duration_ : hint_;
        uint64_t at = start_;
        if (seconds > 0 && d > 0) {
            const double fraction = std::min(1.0, seconds / d);
            double byte_fraction = fraction;
            if (has_toc_) {
                const double p = fraction * 100;
                const int i = std::min(99, (int)p);
                const double a = toc_[i], b = i < 99 ? toc_[i + 1] : 256;
                byte_fraction = (a + (b - a) * (p - i)) / 256;
            }
            at = start_ + (uint64_t)(byte_fraction * (end_ - start_));
        }
        mp3dec_init(&dec_);  // the bit reservoir of the old position is useless
        at_ = at;
        buf_.clear();
        pos_ = 0;
        position_ = seconds;
        return true;
    }

    int decode(std::vector<float>& out) override {
        for (;;) {
            if (buf_.size() - pos_ < 16 * 1024 && at_ + buf_.size() < end_) fill();
            if (pos_ >= buf_.size()) return 0;
            mp3dec_frame_info_t info{};
            const int samples = mp3dec_decode_frame(&dec_, buf_.data() + pos_, (int)(buf_.size() - pos_), pcm_, &info);
            if (!info.frame_bytes) {
                if (at_ + buf_.size() >= end_) return 0;
                // Not enough data for a frame here: read more.
                if (buf_.size() - pos_ >= 64 * 1024) pos_ += 1;
                fill();
                continue;
            }
            pos_ += info.frame_bytes;
            if (samples <= 0) continue;
            if (info.hz && info.hz != rate_) rate_ = info.hz;
            if (info.channels == channels_) {
                out.insert(out.end(), pcm_, pcm_ + samples * channels_);
            } else {
                // A channel-mode change mid-stream: keep the first one.
                for (int i = 0; i < samples; ++i)
                    for (int c = 0; c < channels_; ++c) out.push_back(pcm_[i * info.channels + std::min(c, info.channels - 1)]);
            }
            position_ += (double)samples / rate_;
            return samples;
        }
    }

private:
    // Keeps what has not been decoded and reads up to 64 KB more after it.
    void fill() {
        if (pos_) {
            buf_.erase(buf_.begin(), buf_.begin() + (long)pos_);
            at_ += pos_;
            pos_ = 0;
        }
        const size_t have = buf_.size();
        const uint64_t from = at_ + have;
        if (from >= end_) return;
        const size_t want = (size_t)std::min<uint64_t>(64 * 1024, end_ - from);
        buf_.resize(have + want);
        const size_t got = source_->read_at(from, buf_.data() + have, want);
        buf_.resize(have + got);
    }

    void read_xing(uint64_t frame_at, const mp3dec_frame_info_t& info) {
        const bool mpeg1 = info.hz >= 32000;
        const int side = mpeg1 ? (info.channels == 1 ? 17 : 32) : (info.channels == 1 ? 9 : 17);
        uint8_t x[120];
        if (source_->read_at(frame_at + 4 + side, x, sizeof x) != sizeof x) return;
        if (memcmp(x, "Xing", 4) && memcmp(x, "Info", 4)) return;
        const uint32_t flags = (uint32_t)x[4] << 24 | x[5] << 16 | x[6] << 8 | x[7];
        size_t p = 8;
        if (flags & 1) {
            xing_frames_ = (uint32_t)x[p] << 24 | x[p + 1] << 16 | x[p + 2] << 8 | x[p + 3];
            p += 4;
        }
        if (flags & 2) p += 4;
        if ((flags & 4) && p + 100 <= sizeof x) {
            memcpy(toc_, x + p, 100);
            has_toc_ = true;
        }
    }

    std::shared_ptr<ByteSource> source_;
    mp3dec_t dec_{};
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    uint64_t at_ = 0, start_ = 0, end_ = 0;
    uint32_t xing_frames_ = 0;
    int bitrate_ = 0;
    uint8_t toc_[100] = {};
    bool has_toc_ = false;
    float pcm_[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

}  // namespace

std::unique_ptr<Decoder> make_mp3_decoder(std::shared_ptr<ByteSource> source) { return std::make_unique<Mp3Decoder>(source); }

}  // namespace audio
