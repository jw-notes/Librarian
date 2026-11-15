// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "audio/dsp.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace audio {

void to_stereo(const float* in, size_t frames, int channels, std::vector<float>& out) {
    const size_t at = out.size();
    out.resize(at + frames * 2);
    float* o = out.data() + at;
    if (channels == 2) {
        memcpy(o, in, frames * 2 * sizeof(float));
    } else if (channels == 1) {
        for (size_t i = 0; i < frames; ++i) o[2 * i] = o[2 * i + 1] = in[i];
    } else {
        for (size_t i = 0; i < frames; ++i) {
            o[2 * i] = in[i * channels];
            o[2 * i + 1] = in[i * channels + 1];
        }
    }
}

// ---- resampling ---------------------------------------------------------------------------

void Resampler::set_rate(int in_rate) {
    if (in_rate <= 0 || in_rate == rate_) return;
    rate_ = in_rate;
    reset();
}

void Resampler::reset() {
    step_ = (double)rate_ / kOutputRate;
    pos_ = 1;
    primed_ = false;
    memset(history_, 0, sizeof history_);
}

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
    if (rate_ == kOutputRate) {
        out.insert(out.end(), in, in + frames * 2);
        return;
    }
    if (!frames) return;
    if (!primed_) {
        // Start on the first sample rather than on silence.
        for (int k = 0; k < 3; ++k) {
            history_[2 * k] = in[0];
            history_[2 * k + 1] = in[1];
        }
        primed_ = true;
    }
    // Three frames of history, then the new input.
    work_.assign(history_, history_ + 6);
    work_.insert(work_.end(), in, in + frames * 2);
    const size_t total = work_.size() / 2;
    const float* y = work_.data();
    // Output points between frames 1 and total-2 (each needs one before, two after).
    while (pos_ < (double)(total - 2)) {
        const size_t i = (size_t)pos_;
        const float f = (float)(pos_ - i);
        for (int c = 0; c < 2; ++c) {
            const float y0 = y[2 * (i - 1) + c], y1 = y[2 * i + c], y2 = y[2 * (i + 1) + c], y3 = y[2 * (i + 2) + c];
            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
            const float cc = -0.5f * y0 + 0.5f * y2;
            out.push_back(((a * f + b) * f + cc) * f + y1);
        }
        pos_ += step_;
    }
    pos_ -= (double)(total - 3);
    memcpy(history_, y + 2 * (total - 3), sizeof history_);
}

// ---- time-stretching ----------------------------------------------------------------------

TimeStretch::TimeStretch() : acc_(2 * kN, 0.f), window_(kN) {
    // A periodic Hann window: at half overlap the windows add up to exactly one.
    const float pi = 3.14159265358979f;
    for (int i = 0; i < kN; ++i) window_[i] = 0.5f - 0.5f * cosf(2.f * pi * i / kN);
}

void TimeStretch::finish() {
    if (bypass()) return;
    in_.resize(in_.size() + 2 * (kSearch + kN), 0.f);
}

void TimeStretch::set_speed(double speed) {
    speed = std::max(0.5, std::min(3.0, speed));
    // Leaving or entering the exact-copy mode starts the overlap afresh.
    const bool was = bypass();
    speed_ = speed;
    if (was != bypass()) {
        const uint64_t at = (uint64_t)input_position();
        // Keep the input not yet played.
        const size_t keep_from = (size_t)std::min<uint64_t>(at > dropped_ ? at - dropped_ : 0, in_.size() / 2);
        in_.erase(in_.begin(), in_.begin() + 2 * keep_from);
        dropped_ += keep_from;
        nominal_ = 0;
        previous_ = -1;
        std::fill(acc_.begin(), acc_.end(), 0.f);
    }
}

void TimeStretch::reset() {
    in_.clear();
    std::fill(acc_.begin(), acc_.end(), 0.f);
    nominal_ = 0;
    previous_ = -1;
    dropped_ = 0;
}

void TimeStretch::push(const float* stereo, size_t frames) { in_.insert(in_.end(), stereo, stereo + frames * 2); }

double TimeStretch::input_position() const { return (double)dropped_ + nominal_; }

int TimeStretch::best_offset(int nominal, int natural) const {
    // Correlate a mono mix, coarsely first (every 4th sample and offset), then
    // refine around the best match.
    const float* x = in_.data();
    auto score = [&](int c, int step) {
        double dot = 0, energy = 1e-9;
        for (int i = 0; i < kHop; i += step) {
            const float a = x[2 * (c + i)] + x[2 * (c + i) + 1];
            const float b = x[2 * (natural + i)] + x[2 * (natural + i) + 1];
            dot += a * b;
            energy += a * a;
        }
        return dot / sqrt(energy);
    };
    const int lo = std::max(0, nominal - kSearch), hi = nominal + kSearch;
    int best = std::max(lo, std::min(hi, nominal));
    double best_score = -1e30;
    for (int c = lo; c <= hi; c += 4) {
        const double s = score(c, 4);
        if (s > best_score) {
            best_score = s;
            best = c;
        }
    }
    const int coarse = best;
    best_score = -1e30;
    for (int c = std::max(lo, coarse - 4); c <= std::min(hi, coarse + 4); ++c) {
        const double s = score(c, 1);
        if (s > best_score) {
            best_score = s;
            best = c;
        }
    }
    return best;
}

size_t TimeStretch::pull(std::vector<float>& out) {
    size_t produced = 0;
    if (bypass()) {
        const size_t frames = in_.size() / 2;
        out.insert(out.end(), in_.begin(), in_.end());
        in_.clear();
        dropped_ += frames;
        nominal_ = 0;
        return frames;
    }
    for (;;) {
        const size_t have = in_.size() / 2;
        const int nominal = (int)lround(nominal_);
        const int natural = previous_ < 0 ? 0 : previous_ + kHop;
        if ((size_t)(nominal + kSearch + kN) > have || (size_t)(natural + kN) > have) break;
        const int c = previous_ < 0 ? nominal : best_offset(nominal, natural);
        const float* seg = in_.data() + 2 * c;
        for (int i = 0; i < kN; ++i) {
            acc_[2 * i] += window_[i] * seg[2 * i];
            acc_[2 * i + 1] += window_[i] * seg[2 * i + 1];
        }
        out.insert(out.end(), acc_.begin(), acc_.begin() + 2 * kHop);
        produced += kHop;
        std::copy(acc_.begin() + 2 * kHop, acc_.end(), acc_.begin());
        std::fill(acc_.begin() + 2 * (kN - kHop), acc_.end(), 0.f);
        previous_ = c;
        nominal_ += kHop * speed_;
        // Drop input that no later segment can use.
        const int drop = std::min(previous_, (int)nominal_ - kSearch);
        if (drop > 8192) {
            in_.erase(in_.begin(), in_.begin() + 2 * drop);
            dropped_ += drop;
            previous_ -= drop;
            nominal_ -= drop;
        }
    }
    return produced;
}

}  // namespace audio
