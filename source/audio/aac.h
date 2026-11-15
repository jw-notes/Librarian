// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// FAAD2 behind a small interface, and ADTS framing: shared by the MP4, raw AAC
// and HLS decoders.
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace audio {

class Faad {
public:
    Faad() = default;
    Faad(const Faad&) = delete;
    Faad& operator=(const Faad&) = delete;
    ~Faad();
    // From an MP4's AudioSpecificConfig, or from the first ADTS header.
    bool init_config(const uint8_t* config, size_t size, int& rate, int& channels);
    bool init_adts(const uint8_t* frame, size_t size, int& rate, int& channels);
    bool ready() const { return handle_ != nullptr; }
    // Decodes one access unit (an ADTS frame including its header, or an MP4
    // sample); appends interleaved float frames. Returns the frame count (0 for
    // a frame that produced no output yet), -1 on an error.
    int decode(const uint8_t* data, size_t size, std::vector<float>& out, int& rate, int& channels);
    // After a jump: forget the previous frame's state.
    void reset(long frame);
    std::string last_error;

private:
    bool create();
    void* handle_ = nullptr;
};

// The length of the ADTS frame at `p` (header included), 0 if there is none.
size_t adts_frame_length(const uint8_t* p, size_t available);
// Moves `pos` to the next ADTS frame header confirmed by the one after it.
bool adts_sync(const uint8_t* data, size_t size, size_t& pos);

}  // namespace audio
