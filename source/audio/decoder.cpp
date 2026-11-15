// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// Choosing a decoder, and the HLS path: a playlist of MPEG-TS segments, each
// fetched whole (a few seconds, tens of KB) and demuxed to AAC or MP3 frames.
#include "audio/decoder.h"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "audio/aac.h"
#include "core/log.h"
#include "core/text.h"
#include "net/url.h"

#define MINIMP3_FLOAT_OUTPUT
#include "minimp3/minimp3.h"

namespace audio {

std::unique_ptr<Decoder> make_mp4_decoder(std::shared_ptr<ByteSource> source);
std::unique_ptr<Decoder> make_adts_decoder(std::shared_ptr<ByteSource> source);
std::unique_ptr<Decoder> make_mp3_decoder(std::shared_ptr<ByteSource> source);
std::unique_ptr<Decoder> make_flac_decoder(std::shared_ptr<ByteSource> source);
std::unique_ptr<Decoder> make_ogg_decoder(std::shared_ptr<ByteSource> source);

bool direct_play_mime(const std::string& mime) {
    return mime == "audio/mpeg" || mime == "audio/mp4" || mime == "audio/aac" || mime == "audio/flac" ||
           mime == "audio/ogg" || mime == "audio/opus";
}

namespace {

// A stretch of another decoder's stream, timed from its own start.
class ClipDecoder : public Decoder {
public:
    ClipDecoder(std::unique_ptr<Decoder> inner, double begin, double end)
        : inner_(std::move(inner)), begin_(begin), end_(end) {
        rate_ = inner_->rate();
        channels_ = inner_->channels();
        duration_ = end_ - begin_;
    }
    bool open(std::string&) override { return true; }  // the inner one is open
    bool seek(double seconds) override {
        const double at = begin_ + std::max(0.0, seconds);
        if (!inner_->seek(at)) return false;
        want_ = at;
        position_ = std::max(0.0, inner_->position() - begin_);
        return true;
    }
    int decode(std::vector<float>& out) override {
        while (true) {
            const double before = inner_->position();
            if (before >= end_) return 0;
            std::vector<float> pcm;
            const int n = inner_->decode(pcm);
            if (n <= 0) {
                error_ = inner_->error();
                return n;
            }
            rate_ = inner_->rate();
            channels_ = std::max(1, inner_->channels());
            // Before the clip (the decoder stopped short of it), or past its end: cut.
            int first = std::max(0, std::min(n, (int)((want_ - before) * rate_ + 0.5)));
            int last = std::max(0, std::min(n, (int)((end_ - before) * rate_ + 0.5)));
            want_ = 0;
            if (last <= first) {
                if (before + (double)n / rate_ >= end_) return 0;
                continue;
            }
            out.insert(out.end(), pcm.begin() + (size_t)first * channels_, pcm.begin() + (size_t)last * channels_);
            position_ = before + (double)last / rate_ - begin_;
            return last - first;
        }
    }
    void cancel() override { inner_->cancel(); }
    const char* name() const override { return inner_->name(); }

private:
    std::unique_ptr<Decoder> inner_;
    double begin_, end_, want_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> make_clip_decoder(std::unique_ptr<Decoder> inner, double begin, double end) {
    auto d = std::make_unique<ClipDecoder>(std::move(inner), begin, end);
    d->seek(0);
    return d;
}

std::unique_ptr<Decoder> make_decoder(std::shared_ptr<ByteSource> source, const std::string& mime) {
    uint8_t h[12] = {};
    const size_t n = source->read_at(0, h, sizeof h);
    if (n >= 4 && !memcmp(h, "fLaC", 4)) return make_flac_decoder(source);
    if (n >= 4 && !memcmp(h, "OggS", 4)) return make_ogg_decoder(source);
    if (n >= 8 && (!memcmp(h + 4, "ftyp", 4) || !memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "mdat", 4)))
        return make_mp4_decoder(source);
    if (n >= 3 && !memcmp(h, "ID3", 3)) return mime == "audio/aac" ? make_adts_decoder(source) : make_mp3_decoder(source);
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) return make_adts_decoder(source);
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return make_mp3_decoder(source);
    if (mime == "audio/mpeg") return make_mp3_decoder(source);
    if (mime == "audio/mp4") return make_mp4_decoder(source);
    if (mime == "audio/aac") return make_adts_decoder(source);
    if (mime == "audio/flac") return make_flac_decoder(source);
    if (mime == "audio/ogg" || mime == "audio/opus") return make_ogg_decoder(source);
    return nullptr;
}

// ---- MPEG-TS -----------------------------------------------------------------------------

bool ts_audio_payload(const std::string& ts, std::string& payload, int& stream_type) {
    payload.clear();
    stream_type = 0;
    int pmt_pid = -1, audio_pid = -1;
    const uint8_t* d = (const uint8_t*)ts.data();
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
        const uint8_t* p = d + i;
        if (p[0] != 0x47) {
            // Lost sync: find the next packet start.
            size_t j = i + 1;
            while (j + 188 <= ts.size() && !(d[j] == 0x47 && (j + 188 >= ts.size() || d[j + 188] == 0x47))) ++j;
            if (j + 188 > ts.size()) break;
            i = j - 188;
            continue;
        }
        const int pid = (p[1] & 0x1F) << 8 | p[2];
        const bool start = p[1] & 0x40;
        const int adaptation = (p[3] >> 4) & 3;
        size_t off = 4;
        if (adaptation & 2) off += 1 + p[4];
        if (!(adaptation & 1) || off >= 188) continue;
        const uint8_t* q = p + off;
        size_t len = 188 - off;
        if (pid == 0 && start && audio_pid < 0) {
            // PAT: the first programme's PMT.
            const size_t ptr = q[0];
            if (1 + ptr + 12 > len) continue;
            const uint8_t* s = q + 1 + ptr;
            pmt_pid = (s[10] & 0x1F) << 8 | s[11];
        } else if (pid == pmt_pid && start && audio_pid < 0) {
            const size_t ptr = q[0];
            if (1 + ptr + 12 > len) continue;
            const uint8_t* s = q + 1 + ptr;
            const size_t section = ((s[1] & 0x0F) << 8 | s[2]) + 3;
            const size_t info = (s[10] & 0x0F) << 8 | s[11];
            for (size_t j = 12 + info; j + 5 <= std::min(section - 4, len - 1 - ptr);) {
                const int type = s[j], es_pid = (s[j + 1] & 0x1F) << 8 | s[j + 2];
                if (type == 0x0F || type == 0x03 || type == 0x04) {
                    audio_pid = es_pid;
                    stream_type = type;
                    break;
                }
                j += 5 + ((s[j + 3] & 0x0F) << 8 | s[j + 4]);
            }
        } else if (pid == audio_pid) {
            if (start) {
                // PES header: start code, stream id, length, flags, header length.
                if (len < 9 || q[0] != 0 || q[1] != 0 || q[2] != 1) continue;
                const size_t header = 9 + q[8];
                if (header > len) continue;
                q += header;
                len -= header;
            }
            payload.append((const char*)q, len);
        }
    }
    return audio_pid >= 0 && !payload.empty();
}

bool parse_hls_playlist(const std::string& text, const std::string& base_url, std::vector<HlsSegment>& out) {
    out.clear();
    double next = -1, at = 0;
    for (std::string line : lib::split(text, '\n')) {
        line = lib::trim(line);
        if (line.empty()) continue;
        if (lib::starts_with(line, "#EXTINF:")) {
            next = atof(line.c_str() + 8);
        } else if (line[0] != '#' && next >= 0) {
            out.push_back({lib::resolve_url(base_url, line), at, next});
            at += next;
            next = -1;
        }
    }
    return !out.empty();
}

namespace {

class HlsDecoder : public Decoder {
public:
    HlsDecoder(std::string url, HeaderFn headers) : url_(std::move(url)), headers_(std::move(headers)) {}
    const char* name() const override { return "hls"; }
    void cancel() override { cancelled_ = true; }

    bool open(std::string& error) override {
        std::string text;
        // A new transcode takes a moment to write its playlist.
        if (!http_fetch(url_, headers_, text, error, &cancelled_, 30000)) return false;
        if (!parse_hls_playlist(text, url_, segments_)) {
            error = "The server's stream playlist is empty.";
            return false;
        }
        duration_ = segments_.back().start + segments_.back().duration;
        // The format comes from the first segment actually played.
        rate_ = 44100;
        channels_ = 2;
        return true;
    }

    bool seek(double seconds) override {
        int i = 0;
        while (i + 1 < (int)segments_.size() && segments_[i + 1].start <= seconds) ++i;
        segment_ = i - 1;  // decode() loads segment i next
        payload_.clear();
        pos_ = 0;
        position_ = segments_[i].start;
        if (faad_.ready()) faad_.reset(0);
        mp3dec_init(&mp3_);
        return true;
    }

    int decode(std::vector<float>& out) override {
        for (;;) {
            if (pos_ >= payload_.size() && !next_segment()) return error_.empty() ? 0 : -1;
            const uint8_t* d = (const uint8_t*)payload_.data();
            const size_t size = payload_.size();
            int rate = rate_, channels = channels_, frames = 0;
            if (stream_type_ == 0x0F) {
                if (!adts_frame_length(d + pos_, size - pos_) && !adts_sync(d, size, pos_)) {
                    pos_ = size;
                    continue;
                }
                const size_t n = std::min(adts_frame_length(d + pos_, size - pos_), size - pos_);
                if (!faad_.ready() && !faad_.init_adts(d + pos_, size - pos_, rate, channels)) {
                    error_ = faad_.last_error;
                    return -1;
                }
                frames = faad_.decode(d + pos_, n, out, rate, channels);
                pos_ += n;
            } else {
                mp3dec_frame_info_t info{};
                float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
                frames = mp3dec_decode_frame(&mp3_, d + pos_, (int)(size - pos_), pcm, &info);
                if (!info.frame_bytes) {
                    pos_ = size;
                    continue;
                }
                pos_ += info.frame_bytes;
                if (frames > 0) {
                    rate = info.hz;
                    channels = info.channels;
                    out.insert(out.end(), pcm, pcm + frames * channels);
                }
            }
            if (frames <= 0) continue;
            rate_ = rate;
            channels_ = channels;
            position_ += (double)frames / rate_;
            return frames;
        }
    }

private:
    bool next_segment() {
        if (segment_ + 1 >= (int)segments_.size()) return false;
        ++segment_;
        std::string ts, error;
        if (!http_fetch(segments_[segment_].url, headers_, ts, error, &cancelled_)) {
            error_ = "Part " + std::to_string(segment_ + 1) + " of the stream could not be loaded: " + error;
            return false;
        }
        int type = 0;
        if (!ts_audio_payload(ts, payload_, type)) {
            error_ = "The server's stream has no audio Librarian can decode.";
            return false;
        }
        if (stream_type_ && type != stream_type_) faad_.reset(0);
        stream_type_ = type;
        pos_ = 0;
        // Each segment knows its place: small timing errors do not add up.
        position_ = segments_[segment_].start;
        return true;
    }

    std::string url_;
    HeaderFn headers_;
    std::atomic<bool> cancelled_{false};
    std::vector<HlsSegment> segments_;
    int segment_ = -1, stream_type_ = 0;
    std::string payload_;
    size_t pos_ = 0;
    Faad faad_;
    mp3dec_t mp3_{};
};

}  // namespace

std::unique_ptr<Decoder> make_hls_decoder(const std::string& playlist_url, HeaderFn headers) {
    return std::make_unique<HlsDecoder>(playlist_url, std::move(headers));
}

}  // namespace audio
