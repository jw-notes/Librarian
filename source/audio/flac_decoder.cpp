// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// FLAC through dr_flac (native or in Ogg), reading through a ByteSource (a file or
// Range requests).
#include <algorithm>

#include "audio/decoder.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#include "dr_libs/dr_flac.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace audio {

namespace {

class FlacDecoder : public Decoder {
public:
    explicit FlacDecoder(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}
    ~FlacDecoder() override {
        if (flac_) drflac_close(flac_);
    }
    const char* name() const override { return "flac"; }
    void cancel() override { source_->cancel(); }

    bool open(std::string& error) override {
        flac_ = drflac_open(on_read, on_seek, on_tell, this, nullptr);
        if (!flac_) {
            error = source_->error().empty() ? "Not a FLAC file, or it is damaged." : source_->error();
            return false;
        }
        rate_ = (int)flac_->sampleRate;
        channels_ = flac_->channels;
        if (rate_ <= 0 || channels_ <= 0) {
            error = "The FLAC file has no audio.";
            return false;
        }
        duration_ = flac_->totalPCMFrameCount ? (double)flac_->totalPCMFrameCount / rate_ : hint_;
        return true;
    }

    bool seek(double seconds) override {
        const drflac_uint64 frame = (drflac_uint64)(std::max(0.0, seconds) * rate_);
        if (!drflac_seek_to_pcm_frame(flac_, frame)) return false;
        position_ = (double)frame / rate_;
        return true;
    }

    int decode(std::vector<float>& out) override {
        constexpr drflac_uint64 kFrames = 4096;
        const size_t at = out.size();
        out.resize(at + kFrames * channels_);
        const drflac_uint64 n = drflac_read_pcm_frames_f32(flac_, kFrames, out.data() + at);
        out.resize(at + (size_t)n * channels_);
        if (n == 0) {
            if (!source_->error().empty()) {
                error_ = source_->error();
                return -1;
            }
            return 0;
        }
        position_ += (double)n / rate_;
        return (int)n;
    }

private:
    static size_t on_read(void* user, void* out, size_t n) {
        auto* self = (FlacDecoder*)user;
        const size_t got = self->source_->read_at(self->cursor_, out, n);
        self->cursor_ += got;
        return got;
    }
    static drflac_bool32 on_seek(void* user, int offset, drflac_seek_origin origin) {
        auto* self = (FlacDecoder*)user;
        int64_t at = offset;
        if (origin == DRFLAC_SEEK_CUR) at += (int64_t)self->cursor_;
        else if (origin == DRFLAC_SEEK_END) at += (int64_t)self->source_->size();
        if (at < 0 || (uint64_t)at > self->source_->size()) return DRFLAC_FALSE;
        self->cursor_ = (uint64_t)at;
        return DRFLAC_TRUE;
    }
    static drflac_bool32 on_tell(void* user, drflac_int64* cursor) {
        *cursor = (drflac_int64)((FlacDecoder*)user)->cursor_;
        return DRFLAC_TRUE;
    }

    std::shared_ptr<ByteSource> source_;
    drflac* flac_ = nullptr;
    uint64_t cursor_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> make_flac_decoder(std::shared_ptr<ByteSource> source) { return std::make_unique<FlacDecoder>(source); }

}  // namespace audio
