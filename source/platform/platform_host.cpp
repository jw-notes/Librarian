// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The host build (Linux/WSL), for development and automated tests: the frame
// is kept in memory, the controller is driven by a script, and screenshots are
// written as PNG files.
//
// Script steps (separated by spaces):
//   up down left right x o square triangle l1 r1 l2 r2 options   press a button
//   hold:down:600      hold a button for 600 ms
//   stick:r:127:0:500  right stick x/y (-128..127) for 500 ms; l for left
//   wait:500           wait 500 ms
//   idle               wait until background work and image loads finish
//   text:hello_world   the next on-screen keyboard returns this ("_" = space)
//   shot:name          save the next frame as <shots>/name.png
//   usb-out:N          unplug the drive in slot N (its folder stays, empty, as on the console)
//   usb-in:N:M         plug the drive last unplugged from slot N into slot M
//   quit               leave the app
//   crash              crash on purpose (tests the crash log)
#include "platform/host.h"
#include "platform/platform.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <vector>

#include "core/fs.h"
#include "gfx/canvas.h"
#include "core/log.h"
#include "core/usb_ftp.h"
#include "core/text.h"
#include "core/worker.h"
#include "miniz.h"

namespace platform {

namespace {

std::vector<uint32_t> g_canvas;
std::atomic<bool> g_redraw{true};
std::deque<std::string> g_script;
std::string g_shot_dir = ".", g_pending_shot, g_typed;
bool g_have_typed = false, g_quit = false;
uint32_t g_held = 0;
Pad g_sticks;
uint64_t g_step_until = 0;
std::string g_root;

const std::map<std::string, uint32_t> kButtons = {
    {"up", kUp},         {"down", kDown}, {"left", kLeft}, {"right", kRight}, {"x", kCross},
    {"o", kCircle},      {"square", kSquare}, {"triangle", kTriangle}, {"l1", kL1}, {"r1", kR1},
    {"l2", kL2},         {"r2", kR2},     {"options", kOptions}, {"touchpad", kTouchPad}, {"r3", kR3}, {"l3", kL3},
};

// Simulated drives under LIBRARIAN_USB_ROOT: slots usb0..usb7 always exist;
// an unplugged drive waits in .unplugged-N.
void usb_step(const std::string& step) {
    const char* root = getenv("LIBRARIAN_USB_ROOT");
    if (!root) return;
    const auto parts = lib::split(step, ':');
    const std::string base = root;
    if (parts[0] == "usb-out" && parts.size() == 2) {
        const std::string slot = base + "/usb" + parts[1], aside = base + "/.unplugged-" + parts[1];
        if (rename(slot.c_str(), aside.c_str()) == 0) mkdir(slot.c_str(), 0755);
        LOG("host: unplugged %s", slot.c_str());
    } else if (parts[0] == "usb-in" && parts.size() == 3) {
        const std::string aside = base + "/.unplugged-" + parts[1], slot = base + "/usb" + parts[2];
        rmdir(slot.c_str());
        rename(aside.c_str(), slot.c_str());
        LOG("host: plugged into %s", slot.c_str());
    }
}

bool save_png(const std::string& path) {
    const int fw = frame_width(), fh = frame_height();
    std::vector<uint8_t> rgb((size_t)fw * fh * 3);
    for (size_t i = 0; i < (size_t)fw * fh; ++i) {
        const uint32_t p = g_canvas[i];
        rgb[i * 3] = (uint8_t)(p >> 16);
        rgb[i * 3 + 1] = (uint8_t)(p >> 8);
        rgb[i * 3 + 2] = (uint8_t)p;
    }
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), fw, fh, 3, &size, 6, MZ_FALSE);
    if (!png) return false;
    FILE* f = fopen(path.c_str(), "wb");
    const bool ok = f && fwrite(png, 1, size, f) == size;
    if (f) fclose(f);
    mz_free(png);
    return ok;
}

}  // namespace

int g_frame_w = kWidth, g_frame_h = kHeight;

bool init() {
    g_canvas.assign((size_t)kWidth * kHeight, 0xFF000000);
    const char* root = getenv("LIBRARIAN_DATA");
    g_root = root ? root : "/tmp/librarian-host";
    lib::make_dirs(g_root);
    lib::log_open(g_root + "/librarian.log");
    return true;
}

void shutdown() {}

[[noreturn]] void exit_app() { exit(0); }

uint32_t* canvas() { return g_canvas.data(); }

// The host has no TV: LIBRARIAN_RESOLUTION (720, 1080, 2160) plays one.
int output_height() {
    const char* e = getenv("LIBRARIAN_RESOLUTION");
    return e ? atoi(e) : 1080;
}

bool video_start(int preferred) {
    const int h = preferred ? preferred : output_height();
    g_frame_h = h <= 720 ? 720 : h >= 2160 ? 2160 : 1080;
    g_frame_w = g_frame_h * 16 / 9;
    g_canvas.assign((size_t)g_frame_w * g_frame_h, 0xFF000000);
    gfx::set_render_scale((float)g_frame_h / kHeight);
    LOG("video: %dx%d", g_frame_w, g_frame_h);
    return true;
}

int frame_width() { return g_frame_w; }
int frame_height() { return g_frame_h; }

void present() {
    if (!g_pending_shot.empty()) {
        const std::string path = lib::join_path(g_shot_dir, g_pending_shot + ".png");
        LOG(save_png(path) ? "screenshot %s" : "screenshot FAILED %s", path.c_str());
        g_pending_shot.clear();
    }
    // A display flip takes one frame.
    usleep(16000);
}

Pad read_pad() {
    Pad pad;
    const uint64_t now = now_ms();
    if (now < g_step_until) {
        pad = g_sticks;
        pad.buttons = g_held;
        return pad;
    }
    g_held = 0;
    g_sticks = Pad{};
    while (!g_script.empty()) {
        const std::string step = g_script.front();
        if (step == "idle") {
            // Settle: no queued work for a few frames in a row.
            static int quiet = 0;
            if (lib::jobs_in_flight() > 0) { quiet = 0; return pad; }
            if (++quiet < 6) return pad;
            quiet = 0;
            g_script.pop_front();
            continue;
        }
        g_script.pop_front();
        if (step == "quit") { g_quit = true; break; }
        if (step == "crash") {
            LOG("crashing on purpose (script step)");
            *(volatile int*)nullptr = 1;
        }
        if (lib::starts_with(step, "wait:")) {
            g_step_until = now + atoi(step.c_str() + 5);
            return pad;
        }
        if (lib::starts_with(step, "shot:")) {
            g_pending_shot = step.substr(5);
            request_redraw();
            // Give the frame with the screenshot a moment to be drawn.
            g_step_until = now + 50;
            return pad;
        }
        if (lib::starts_with(step, "usb-out:") || lib::starts_with(step, "usb-in:")) {
            usb_step(step);
            continue;
        }
        if (lib::starts_with(step, "text:")) {
            g_typed = lib::replace_all(step.substr(5), "_", " ");
            g_have_typed = true;
            continue;
        }
        if (lib::starts_with(step, "stick:")) {
            const auto parts = lib::split(step, ':');
            if (parts.size() == 5) {
                const int x = std::max(-128, std::min(127, atoi(parts[2].c_str())));
                const int y = std::max(-128, std::min(127, atoi(parts[3].c_str())));
                if (parts[1] == "r") { g_sticks.rx = x; g_sticks.ry = y; }
                else { g_sticks.lx = x; g_sticks.ly = y; }
                g_step_until = now + std::max(1, atoi(parts[4].c_str()));
                return g_sticks;
            }
        }
        if (lib::starts_with(step, "hold:")) {
            const auto parts = lib::split(step, ':');
            auto found = parts.size() == 3 ? kButtons.find(parts[1]) : kButtons.end();
            if (found != kButtons.end()) {
                g_held = found->second;
                g_step_until = now + atoi(parts[2].c_str());
                pad.buttons = g_held;
                return pad;
            }
        }
        auto found = kButtons.find(step);
        if (found != kButtons.end()) {
            // Pressed for two frames, then released for two.
            g_held = found->second;
            g_step_until = now + 34;
            pad.buttons = g_held;
            g_script.push_front("wait:34");
            return pad;
        }
        fprintf(stderr, "unknown script step: %s\n", step.c_str());
    }
    if (g_quit || (g_script.empty() && getenv("LIBRARIAN_EXIT_AT_END"))) {
        g_quit = true;
        pad.buttons = 0;
    }
    return pad;
}

void set_light_bar(uint32_t) {}

namespace {
char g_crash_path[512];

void on_crash(int sig, siginfo_t* info, void* context) {
    const uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
    const uintptr_t pc = context ? (uintptr_t)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP] : 0;
    lib::log_crash(g_crash_path, sig, strsignal(sig), fault, pc, (uintptr_t)&install_crash_handler);
}
}  // namespace

void install_crash_handler() {
    snprintf(g_crash_path, sizeof g_crash_path, "%s/crash.log", data_root().c_str());
    for (int sig : {SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV}) {
        struct sigaction act {};
        act.sa_sigaction = on_crash;
        act.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(sig, &act, nullptr);
    }
}

// LIBRARIAN_USB_FTP=<host:port> reads the simulated slots through an FTP
// server instead (tests/tools/ftp_test_server.py), as the console does when
// GoldHEN cannot lift the sandbox.
bool enable_usb_access() {
    const char* ftp = getenv("LIBRARIAN_USB_FTP");
    const char* root = getenv("LIBRARIAN_USB_ROOT");
    if (ftp && *ftp && root && *root) {
        lib::usb_ftp::enable(ftp, std::string(root) + "/usb");
        LOG("usb: drives go through the FTP server at %s", ftp);
    }
    return true;
}

int utc_offset_seconds() {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return (int)local.tm_gmtoff;
}

bool clock_24h() { return true; }

uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static const uint64_t start = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - start;
}

void sleep_ms(int ms) { usleep(ms * 1000); }

void request_redraw() { g_redraw = true; }
bool take_redraw() { return g_redraw.exchange(false); }

std::string data_root() { return g_root; }

std::string system_language() {
    const char* lang = getenv("LIBRARIAN_LANG");
    return lang && *lang ? lang : "en";
}

std::string device_secret() {
    std::string id;
    lib::read_file("/etc/machine-id", id, 4096);
    return lib::trim(id);
}

std::string assets_root() {
    const char* assets = getenv("LIBRARIAN_ASSETS");
    return assets ? assets : "assets";
}

// LIBRARIAN_USB_ROOT holds usb0 .. usb7, like the console's /mnt.
std::vector<std::string> usb_mount_points() {
    std::vector<std::string> slots;
    if (const char* root = getenv("LIBRARIAN_USB_ROOT"))
        for (int i = 0; i < 8; ++i) slots.push_back(std::string(root) + "/usb" + std::to_string(i));
    return slots;
}

std::vector<std::string> internal_document_dirs() {
    if (const char* docs = getenv("LIBRARIAN_DOCS")) return {docs};
    return {};
}

void background_thread_priority() {}
std::string memory_report() { return "host"; }

// ---- sound --------------------------------------------------------------------------------

namespace {
pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> g_audio_fast{getenv("LIBRARIAN_AUDIO_FAST") != nullptr}, g_audio_capture{false};
std::vector<int16_t> g_audio_captured;
std::atomic<uint64_t> g_audio_played{0};
FILE* g_wav = nullptr;
uint64_t g_wav_bytes = 0, g_audio_next_us = 0;

uint64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void wav_header(FILE* f, uint32_t data) {
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    put32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(2);
    put32(kAudioRate);
    put32(kAudioRate * 4);
    put16(4);
    put16(16);
    fwrite("data", 1, 4, f);
    put32(data);
}
}  // namespace

bool audio_open() {
    pthread_mutex_lock(&g_audio_lock);
    const char* wav = getenv("LIBRARIAN_AUDIO_WAV");
    if (wav && !g_wav && (g_wav = fopen(wav, "wb"))) {
        g_wav_bytes = 0;
        wav_header(g_wav, 0);
    }
    g_audio_next_us = 0;
    pthread_mutex_unlock(&g_audio_lock);
    return true;
}

bool audio_output(const int16_t* block) {
    pthread_mutex_lock(&g_audio_lock);
    if (g_audio_capture) g_audio_captured.insert(g_audio_captured.end(), block, block + 2 * kAudioBlock);
    if (g_wav) g_wav_bytes += fwrite(block, 4, kAudioBlock, g_wav) * 4;
    pthread_mutex_unlock(&g_audio_lock);
    g_audio_played += kAudioBlock;
    if (g_audio_fast) return true;
    // A device takes a block when the previous one has played.
    const uint64_t now = now_us(), period = 1000000ull * kAudioBlock / kAudioRate;
    if (!g_audio_next_us || g_audio_next_us + 200000 < now) g_audio_next_us = now;
    if (g_audio_next_us > now) usleep((useconds_t)(g_audio_next_us - now));
    g_audio_next_us += period;
    return true;
}

void audio_close() {
    pthread_mutex_lock(&g_audio_lock);
    if (g_wav) {
        wav_header(g_wav, (uint32_t)g_wav_bytes);
        fclose(g_wav);
        g_wav = nullptr;
    }
    pthread_mutex_unlock(&g_audio_lock);
}

}  // namespace platform

namespace host {

void set_script(const std::string& script, const std::string& shot_dir) {
    for (const auto& step : lib::split(script, ' '))
        if (!step.empty()) platform::g_script.push_back(step);
    platform::g_shot_dir = shot_dir;
    lib::make_dirs(shot_dir);
}

bool quit_requested() { return platform::g_quit; }

void audio_fast(bool on) { platform::g_audio_fast = on; }
void audio_capture(bool on) {
    pthread_mutex_lock(&platform::g_audio_lock);
    platform::g_audio_capture = on;
    if (!on) platform::g_audio_captured.clear();
    pthread_mutex_unlock(&platform::g_audio_lock);
}
std::vector<int16_t> audio_take() {
    pthread_mutex_lock(&platform::g_audio_lock);
    std::vector<int16_t> out;
    out.swap(platform::g_audio_captured);
    pthread_mutex_unlock(&platform::g_audio_lock);
    return out;
}
uint64_t audio_frames_played() { return platform::g_audio_played; }

bool take_typed_text(std::string& out) {
    if (!platform::g_have_typed) return false;
    platform::g_have_typed = false;
    out = platform::g_typed;
    return true;
}

}  // namespace host
