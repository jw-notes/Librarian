// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Host-only test controls (see platform_host.cpp for the script language).
#ifdef LIB_HOST
#include <stdint.h>

#include <string>
#include <vector>

namespace host {

void set_script(const std::string& script, const std::string& shot_dir);
bool quit_requested();
// Text queued by a "text:" script step for the next on-screen keyboard.
bool take_typed_text(std::string& out);

// Sound: the host plays nothing, at the pace of a real device, and records
// what it was given. Fast: no waiting (tests). The recording can also go to
// a WAV file named by LIBRARIAN_AUDIO_WAV.
void audio_fast(bool on);
void audio_capture(bool on);
std::vector<int16_t> audio_take();
uint64_t audio_frames_played();

}  // namespace host
#endif
