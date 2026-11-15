// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "audio/aac.h"

#include "core/log.h"
#include "faad2/include/neaacdec.h"

namespace audio {

Faad::~Faad() {
    if (handle_) NeAACDecClose((NeAACDecHandle)handle_);
}

bool Faad::create() {
    if (handle_) NeAACDecClose((NeAACDecHandle)handle_);
    handle_ = NeAACDecOpen();
    if (!handle_) return false;
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration((NeAACDecHandle)handle_);
    config->outputFormat = FAAD_FMT_FLOAT;
    config->downMatrix = 1;  // 5.1 becomes stereo
    NeAACDecSetConfiguration((NeAACDecHandle)handle_, config);
    return true;
}

bool Faad::init_config(const uint8_t* data, size_t size, int& rate, int& channels) {
    if (!create()) return false;
    unsigned long r = 0;
    unsigned char c = 0;
    if (NeAACDecInit2((NeAACDecHandle)handle_, (unsigned char*)data, (unsigned long)size, &r, &c) != 0) {
        last_error = "The AAC stream's configuration is not supported.";
        NeAACDecClose((NeAACDecHandle)handle_);
        handle_ = nullptr;
        return false;
    }
    rate = (int)r;
    channels = c > 2 ? 2 : c;
    return true;
}

bool Faad::init_adts(const uint8_t* data, size_t size, int& rate, int& channels) {
    if (!create()) return false;
    unsigned long r = 0;
    unsigned char c = 0;
    if (NeAACDecInit((NeAACDecHandle)handle_, (unsigned char*)data, (unsigned long)size, &r, &c) < 0) {
        last_error = "The AAC stream could not be read.";
        NeAACDecClose((NeAACDecHandle)handle_);
        handle_ = nullptr;
        return false;
    }
    rate = (int)r;
    channels = c > 2 ? 2 : c;
    return true;
}

int Faad::decode(const uint8_t* data, size_t size, std::vector<float>& out, int& rate, int& channels) {
    if (!handle_) return -1;
    NeAACDecFrameInfo info{};
    const float* pcm = (const float*)NeAACDecDecode((NeAACDecHandle)handle_, &info, (unsigned char*)data, (unsigned long)size);
    if (info.error) {
        last_error = NeAACDecGetErrorMessage(info.error);
        return -1;
    }
    if (!pcm || !info.samples || !info.channels) return 0;
    rate = (int)info.samplerate;
    channels = info.channels;
    const size_t frames = info.samples / info.channels;
    out.insert(out.end(), pcm, pcm + frames * info.channels);
    return (int)frames;
}

void Faad::reset(long frame) {
    if (handle_) NeAACDecPostSeekReset((NeAACDecHandle)handle_, frame);
}

size_t adts_frame_length(const uint8_t* p, size_t available) {
    if (available < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    const size_t length = ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) | (p[5] >> 5);
    return length >= 7 ? length : 0;
}

bool adts_sync(const uint8_t* data, size_t size, size_t& pos) {
    for (; pos + 7 <= size; ++pos) {
        const size_t n = adts_frame_length(data + pos, size - pos);
        if (!n) continue;
        // The next frame must follow, unless this one ends the buffer.
        if (pos + n == size || (pos + n + 7 <= size && adts_frame_length(data + pos + n, size - pos - n))) return true;
    }
    return false;
}

}  // namespace audio
