// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Controller input for one frame: presses, auto-repeat for held directions and
// shoulder buttons, and the analog sticks (the left stick acts as the D-pad).
#include <stdint.h>

#include "platform/platform.h"

namespace ui {

struct Input {
    uint32_t held = 0;
    uint32_t pressed = 0;   // went down this frame
    uint32_t repeated = 0;  // pressed, or auto-repeating while held
    uint32_t digital = 0;  // physical buttons, without left-stick D-pad emulation
    int lx = 0, ly = 0;
    int rx = 0, ry = 0;     // right stick, dead zone removed, -127..127

    bool press(uint32_t b) const { return pressed & b; }
    bool repeat(uint32_t b) const { return repeated & b; }
    bool down(uint32_t b) const { return held & b; }
};

int stick_axis(int value, int deadzone = 18);

class InputReader {
public:
    Input read();
    // Swallow buttons that are currently held (after a screen change, so the
    // press that opened a screen does not also act on it).
    void reset();

private:
    uint32_t previous_ = 0;
    uint64_t repeat_at_ = 0;
    int repeat_count_ = 0;
    uint32_t ignore_ = 0;
};

}  // namespace ui
