// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/input.h"

#include <stdlib.h>
#include <algorithm>

namespace ui {

using namespace platform;

namespace {
constexpr uint32_t kRepeatable = kUp | kDown | kLeft | kRight | kL1 | kR1 | kL2 | kR2;

}  // namespace

int stick_axis(int value, int deadzone) {
    const int magnitude = std::min(127, std::abs(value));
    return magnitude <= deadzone ? 0 : (value < 0 ? -1 : 1) * (magnitude - deadzone) * 127 / (127 - deadzone);
}

Input InputReader::read() {
    const Pad pad = read_pad();
    uint32_t held = pad.buttons;
    // The left stick as a D-pad.
    if (pad.lx < -64) held |= kLeft;
    if (pad.lx > 64) held |= kRight;
    if (pad.ly < -64) held |= kUp;
    if (pad.ly > 64) held |= kDown;
    ignore_ &= held;  // a released button is live again
    held &= ~ignore_;

    Input in;
    in.held = held;
    in.digital = pad.buttons & ~ignore_;
    in.pressed = held & ~previous_;
    in.repeated = in.pressed;
    const uint64_t now = now_ms();
    if (in.pressed & kRepeatable) {
        repeat_at_ = now + 380;
        repeat_count_ = 0;
    } else if ((held & kRepeatable) && (held & kRepeatable) == (previous_ & kRepeatable) && now >= repeat_at_) {
        in.repeated |= held & kRepeatable;
        // Speeds up the longer it is held.
        ++repeat_count_;
        repeat_at_ = now + (repeat_count_ > 12 ? 40 : repeat_count_ > 4 ? 70 : 110);
    }
    previous_ = held;
    in.lx = stick_axis(pad.lx);
    in.ly = stick_axis(pad.ly);
    in.rx = stick_axis(pad.rx);
    in.ry = stick_axis(pad.ry);
    return in;
}

void InputReader::reset() { ignore_ = previous_; }

}  // namespace ui
