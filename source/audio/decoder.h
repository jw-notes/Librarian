// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Audio decoders: MP3 (minimp3), AAC in MP4/M4A/M4B or as ADTS (FAAD2, with
// Librarian's own MP4 demuxer), FLAC (dr_flac, also in Ogg), Opus (libopus) and
// Vorbis (stb_vorbis) in Ogg (ogg_decoder.cpp), and HLS playlists of MPEG-TS
// segments (what Audiobookshelf sends when it transcodes another format).
//
// Output is interleaved float samples at the stream's own rate and channel
// count; the player converts to the console's 48 kHz stereo.
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "audio/byte_source.h"

namespace audio {

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual bool open(std::string& error) = 0;
    // Moves to `seconds` or a little before it (a frame boundary); position()
    // then says exactly where the next decoded sample is.
    virtual bool seek(double seconds) = 0;
    // Appends interleaved frames (channels() samples each) to `out`. Returns
    // the frames added, 0 at the end of the stream, -1 on an error (error()).
    virtual int decode(std::vector<float>& out) = 0;
    // Known after open(); AAC with SBR can change the rate after the first frame.
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    // Total length in seconds (0 if the stream does not say).
    virtual double duration() const { return duration_; }
    double position() const { return position_; }
    const std::string& error() const { return error_; }
    // The length the server measured, used where the stream itself has no
    // index (MP3 without a Xing table): seeking then interpolates on it.
    void set_duration_hint(double seconds) { hint_ = seconds; }
    // Makes a network read in progress give up (the player is seeking or closing).
    virtual void cancel() = 0;
    virtual const char* name() const = 0;

protected:
    int rate_ = 0, channels_ = 0;
    double duration_ = 0, position_ = 0, hint_ = 0;
    std::string error_;
};

// Picks a decoder from the first bytes (the MIME type is only a hint).
std::unique_ptr<Decoder> make_decoder(std::shared_ptr<ByteSource> source, const std::string& mime);
// Plays only [begin, end) seconds of an opened decoder, as if that were the
// whole stream (read-along narration clips).
std::unique_ptr<Decoder> make_clip_decoder(std::unique_ptr<Decoder> inner, double begin, double end);
// An HLS playlist (.m3u8) of MPEG-TS segments.
std::unique_ptr<Decoder> make_hls_decoder(const std::string& playlist_url, HeaderFn headers);

// Formats Audiobookshelf may send for direct play (its MIME types).
bool direct_play_mime(const std::string& mime);

// ---- parts shared by the decoders and used by the tests ---------------------------------

// Concatenates the payload of the first audio stream in an MPEG-TS buffer.
// `stream_type`: 0x0F AAC (ADTS), 0x03/0x04 MPEG audio. False if none.
bool ts_audio_payload(const std::string& ts, std::string& payload, int& stream_type);

struct HlsSegment {
    std::string url;
    double start = 0, duration = 0;
};
// Parses a media playlist; segment addresses are made absolute against `base_url`.
bool parse_hls_playlist(const std::string& text, const std::string& base_url, std::vector<HlsSegment>& out);

}  // namespace audio
