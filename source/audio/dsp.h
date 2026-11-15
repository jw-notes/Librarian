// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Sound processing between the decoders and the console: conversion to the
// 48 kHz stereo the PS4 plays, and a time-stretcher so a faster or slower
// speed keeps voices at their natural pitch.
#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace audio {

constexpr int kOutputRate = 48000;

// Any channel count to stereo (mono is doubled; more channels keep the first two).
void to_stereo(const float* in, size_t frames, int channels, std::vector<float>& out);

// Streaming cubic (Catmull-Rom) resampler of stereo frames to 48 kHz.
class Resampler {
public:
    // A new input rate restarts it.
    void set_rate(int in_rate);
    void reset();
    void process(const float* stereo, size_t frames, std::vector<float>& out);

private:
    int rate_ = kOutputRate;
    double step_ = 1, pos_ = 1;
    std::vector<float> work_;
    float history_[6] = {};  // the last three input frames
    bool primed_ = false;
};

// WSOLA (waveform-similarity overlap-add) time-stretching of 48 kHz stereo.
// Output segments are taken from the input `speed` times as fast, each one
// placed where it best continues the previous one, so pitch is unchanged.
class TimeStretch {
public:
    TimeStretch();
    void set_speed(double speed);
    double speed() const { return speed_; }
    void reset();
    void push(const float* stereo, size_t frames);
    // Appends what can be produced now; returns the frame count.
    size_t pull(std::vector<float>& out);
    // The input has ended: lets the last segments out (followed by silence).
    void finish();
    // The input frame (counted from the last reset) that the next output frame
    // plays: maps output back to the book's time.
    double input_position() const;

private:
    static constexpr int kN = 1024, kHop = kN / 2, kSearch = 480;
    bool bypass() const { return speed_ > 0.999 && speed_ < 1.001; }
    int best_offset(int nominal, int natural) const;

    double speed_ = 1;
    std::vector<float> in_, acc_, window_;
    double nominal_ = 0;  // where the next segment nominally starts in in_
    int previous_ = -1;   // where the last one started in in_
    uint64_t dropped_ = 0;  // input frames removed from the front of in_
};

}  // namespace audio
