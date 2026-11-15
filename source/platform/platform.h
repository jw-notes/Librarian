// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Everything that differs between the console and the host build: display,
// controller, clocks, paths and thread priorities. The rest of the program
// only talks to the console through this header and net/http.h.
#include <stdint.h>

#include <string>
#include <vector>

namespace platform {

// The logical canvas every screen is laid out on; the frame itself has the
// TV's resolution (frame_width x frame_height, see gfx/canvas.h).
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// DualShock 4 buttons, using the console's own bit values.
enum Button : uint32_t {
    kL3 = 0x0002,
    kR3 = 0x0004,
    kOptions = 0x0008,
    kUp = 0x0010,
    kRight = 0x0020,
    kDown = 0x0040,
    kLeft = 0x0080,
    kL2 = 0x0100,
    kR2 = 0x0200,
    kL1 = 0x0400,
    kR1 = 0x0800,
    kTriangle = 0x1000,
    kCircle = 0x2000,
    kCross = 0x4000,
    kSquare = 0x8000,
    kTouchPad = 0x100000,
};

struct Pad {
    uint32_t buttons = 0;
    // Sticks centred on 0, -128..127; +x right, +y down.
    int lx = 0, ly = 0, rx = 0, ry = 0;
    bool connected = true;
};

bool init();
void shutdown();
[[noreturn]] void exit_app();

// Sets up the frame, after Settings are loaded: `preferred` is the height
// asked for (720, 1080, 2160) or 0 for the TV's own. Falls back to 1080p when
// the console refuses a size. Also sets gfx::render_scale.
bool video_start(int preferred);
// The frame's real size, and the TV's output height (0 when not known).
int frame_width();
int frame_height();
int output_height();
// The frame being drawn: frame_width * frame_height pixels, 0xAARRGGBB, in
// cached memory.
uint32_t* canvas();
// Shows the canvas and waits for the flip.
void present();
Pad read_pad();
void set_light_bar(uint32_t rgb);

// Local time: the console's C library knows no time zone, so the offset
// (including summer time) and the 12/24-hour preference come from system settings.
int utc_offset_seconds();
bool clock_24h();

// Real elapsed time since launch.
uint64_t now_ms();
void sleep_ms(int ms);

// Something changed off the UI thread and the screen should be redrawn.
void request_redraw();
bool take_redraw();

// Writable per-user data: /data/Librarian on the console.
std::string data_root();
// Bytes that identify this console and stay the same (its OpenPSID on the
// PS4; /etc/machine-id on the host), mixed into the key that encrypts
// settings.json, so a copy of the file cannot be read elsewhere. "" when
// nothing is available.
std::string device_secret();
// The console's system language as Librarian's language code ("en", "ja",
// "pt-BR", "de", "fr", "es"); "en" for any other. Host: LIBRARIAN_LANG.
std::string system_language();
// Read-only files shipped in the package (fonts): /app0/assets on the console.
std::string assets_root();
// Lets the app read USB drives. Inside the console's app sandbox /mnt/usbN
// may look empty; under GoldHEN the app can step out of the sandbox. Called
// after the package's own files (fonts) are loaded. False if not possible.
bool enable_usb_access();
// Writes <data>/crash.log (signal, addresses, last log lines) when the app
// crashes. Once, at startup, after init().
void install_crash_handler();

// The USB slots, mounted or not: /mnt/usb0 .. /mnt/usb7 on the console. All
// eight folders always exist; an empty one has no drive in it.
std::vector<std::string> usb_mount_points();
// Folders on the console's own storage to look for documents in (/data/books).
std::vector<std::string> internal_document_dirs();

// Sound output: 48 kHz, 16-bit stereo, in blocks of kAudioBlock frames. Used
// from the player's own output thread.
constexpr int kAudioRate = 48000;
constexpr int kAudioBlock = 1024;
bool audio_open();
// Queues one block (kAudioBlock * 2 samples); waits while the device is full,
// which paces the caller in real time. False if the device failed.
bool audio_output(const int16_t* block);
void audio_close();

void background_thread_priority();
// "free memory 812 MB", for the diagnostic log.
std::string memory_report();

}  // namespace platform
