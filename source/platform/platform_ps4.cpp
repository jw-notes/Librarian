// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// The console: sceVideoOut scanout, scePad input, /data storage.
// Several details here were learnt the hard way and are kept:
// the clock id translation, drawing into cached memory, and the controller
// reconnect loop.
#include "platform/platform.h"

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "core/fs.h"
#include "core/log.h"
#include "core/usb_ftp.h"
#include "gfx/canvas.h"
#include "platform/sandbox_ps4.h"

extern "C" uint64_t sceKernelGetProcessTime(void);
// The toolchain declares this without its argument.
extern "C" int32_t lib_flexible_memory_free(size_t* size) __asm__("sceKernelAvailableFlexibleMemorySize");

// clock_gettime comes from the console's libkernel, which numbers clocks the
// FreeBSD way, while <time.h> uses Linux's numbers: CLOCK_MONOTONIC (1)
// reaches the kernel as CLOCK_VIRTUAL, the process's CPU time. Every caller is
// linked through this (--wrap=clock_gettime), which translates the ids.
extern "C" int __real_clock_gettime(int id, struct timespec* ts);
extern "C" int __wrap_clock_gettime(int id, struct timespec* ts) {
    switch (id) {
        case 1: case 4: id = 4; break;  // MONOTONIC, MONOTONIC_RAW -> CLOCK_MONOTONIC
        case 2: id = 15; break;         // PROCESS_CPUTIME_ID
        case 3: id = 14; break;         // THREAD_CPUTIME_ID
        case 7: id = 5; break;          // BOOTTIME -> CLOCK_UPTIME
        default: break;                 // REALTIME is 0 in both
    }
    return __real_clock_gettime(id, ts);
}

// USB drives live outside the PlayStation app sandbox; platform/sandbox_ps4.h
// leaves it with GoldHEN's escape.

namespace {

// What each USB slot looks like to the app, for the log.
void log_usb_slots(const char* when) {
    for (int i = 0; i < 8; ++i) {
        const std::string slot = "/mnt/usb" + std::to_string(i);
        errno = 0;
        DIR* dir = opendir(slot.c_str());
        if (!dir) {
            LOG("usb (%s): %s cannot be opened (errno %d)", when, slot.c_str(), errno);
            continue;
        }
        int entries = 0;
        while (dirent* e = readdir(dir))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) ++entries;
        closedir(dir);
        LOG("usb (%s): %s has %d entries", when, slot.c_str(), entries);
    }
}

}  // namespace

namespace platform {

namespace {

// The frame: the TV's resolution (video_start). Two scanout buffers, rounded
// up to the 2 MiB mapping granularity.
int g_frame_w = kWidth, g_frame_h = kHeight, g_output_h = 0;
size_t g_frame_bytes = 0, g_scanout_bytes = 0;

int g_video = -1, g_pad = -1, g_user = 0, g_frame = 0;
uint32_t* g_scanout[2] = {};
void* g_memory = nullptr;
off_t g_memory_offset = 0;
OrbisKernelEqueue g_flip_queue;
bool g_queue = false, g_registered = false, g_allocated = false;
// Drawing happens in ordinary cached memory: the scanout buffers are
// write-combined, and every blend reading them back was uncached.
uint32_t* g_canvas = nullptr;
std::atomic<bool> g_redraw{true};
uint32_t g_light = 0xFFFFFFFF;
std::string g_data_root;

}  // namespace

bool init() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    sceUserServiceInitialize(nullptr);
    if (sceUserServiceGetInitialUser(&g_user) < 0) return false;

    // Settings and the library are per console user.
    char root[128];
    snprintf(root, sizeof root, "/data/Librarian/user-%08x", g_user);
    g_data_root = root;
    lib::make_dirs(g_data_root);
    lib::log_open(g_data_root + "/librarian.log");
    LOG("Librarian starting, user %08x", g_user);

    scePadInit();
    g_pad = scePadOpen(g_user, 0, 0, nullptr);
    if (g_pad < 0) { LOG("scePadOpen failed 0x%08x", g_pad); return false; }

    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, 0, 0, nullptr);
    if (g_video < 0) { LOG("sceVideoOutOpen failed 0x%08x", g_video); return false; }
    OrbisVideoOutResolutionStatus status{};
    if (sceVideoOutGetResolutionStatus(g_video, &status) >= 0) {
        g_output_h = (int)status.height;
        LOG("video: the TV gets %ux%u", status.width, status.height);
    }
    return true;
}

namespace {

// Scanout buffers and the drawing canvas for a w x h frame.
bool make_frame(int w, int h) {
    g_frame_w = w;
    g_frame_h = h;
    g_frame_bytes = (size_t)w * h * 4;
    g_scanout_bytes = ((2 * g_frame_bytes) + 0x1FFFFF) & ~(size_t)0x1FFFFF;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), g_scanout_bytes, 0x200000, 3, &g_memory_offset) < 0)
        return false;
    g_allocated = true;
    if (sceKernelMapDirectMemory(&g_memory, g_scanout_bytes, 0x33, 0, g_memory_offset, 0x200000) < 0) return false;
    g_scanout[0] = (uint32_t*)g_memory;
    g_scanout[1] = g_scanout[0] + (size_t)w * h;
    memset(g_memory, 0, 2 * g_frame_bytes);
    OrbisVideoOutBufferAttribute attr{};
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, w, h, w);
    const int rc = sceVideoOutRegisterBuffers(g_video, 0, (void* const*)g_scanout, 2, &attr);
    if (rc < 0) {
        LOG("video: %dx%d refused (0x%08x)", w, h, rc);
        return false;
    }
    g_registered = true;
    void* canvas = nullptr;
    if (posix_memalign(&canvas, 64, g_frame_bytes) != 0) return false;
    g_canvas = (uint32_t*)canvas;
    memset(g_canvas, 0, g_frame_bytes);
    return true;
}

void free_frame() {
    if (g_registered) sceVideoOutUnregisterBuffers(g_video, 0);
    g_registered = false;
    if (g_memory) sceKernelMunmap(g_memory, g_scanout_bytes);
    if (g_allocated) sceKernelReleaseDirectMemory(g_memory_offset, g_scanout_bytes);
    g_memory = nullptr;
    g_allocated = false;
    free(g_canvas);
    g_canvas = nullptr;
}

}  // namespace

bool video_start(int preferred) {
    // The TV's own resolution unless one was chosen: 720p for any smaller
    // output, 2160p only when the console sends 4K (a PS4 Pro).
    const int want = preferred ? preferred : g_output_h ? g_output_h : 1080;
    int h = want <= 720 ? 720 : want >= 2160 ? 2160 : 1080;
    bool ok = make_frame(h * 16 / 9, h);
    if (!ok && h != 1080) {
        free_frame();
        h = 1080;
        ok = make_frame(1920, 1080);
    }
    if (!ok) return false;
    gfx::set_render_scale((float)h / kHeight);
    LOG("video: drawing at %dx%d (%s)", g_frame_w, g_frame_h, preferred ? "chosen in Settings" : "the TV's");
    if (sceKernelCreateEqueue(&g_flip_queue, "Librarian flip") < 0) return false;
    g_queue = true;
    if (sceVideoOutAddFlipEvent(g_flip_queue, g_video, nullptr) < 0) return false;
    sceVideoOutSetFlipRate(g_video, 0);
    sceSystemServiceHideSplashScreen();
    return true;
}

int frame_width() { return g_frame_w; }
int frame_height() { return g_frame_h; }
int output_height() { return g_output_h; }

namespace {
void audio_release();  // below, with the rest of the sound code
int open_port();
}  // namespace

void shutdown() {
    audio_release();
    // Hand the sandbox back before the app closes.
    sandbox::restore();
    LOG("shutdown");
    if (g_pad >= 0) {
        scePadResetLightBar(g_pad);
        scePadClose(g_pad);
        g_pad = -1;
    }
    bool released = true;
    if (g_video >= 0) {
        // Scanout memory must not be freed while a flip is queued.
        for (int i = 0; i < 100 && sceVideoOutIsFlipPending(g_video) > 0; ++i) usleep(10000);
        if (g_registered) sceVideoOutUnregisterBuffers(g_video, 0);
        released = sceVideoOutClose(g_video) >= 0;
        g_video = -1;
    }
    if (g_queue) sceKernelDeleteEqueue(g_flip_queue);
    if (released && g_memory && sceKernelMunmap(g_memory, g_scanout_bytes) >= 0 && g_allocated)
        sceKernelReleaseDirectMemory(g_memory_offset, g_scanout_bytes);
}

[[noreturn]] void exit_app() {
    // Returning from main is not a clean exit for a homebrew title: ask the
    // system to close the app and go back to the home screen.
    sceSystemServiceLoadExec("exit", nullptr);
    _Exit(0);
}

uint32_t* canvas() { return g_canvas; }

void present() {
    memcpy(g_scanout[g_frame], g_canvas, g_frame_bytes);
    if (sceVideoOutSubmitFlip(g_video, g_frame, 1, 0) >= 0) {
        while (sceVideoOutIsFlipPending(g_video) > 0) {
            OrbisKernelEvent event;
            int count;
            sceKernelWaitEqueue(g_flip_queue, &event, 1, &count, nullptr);
        }
    }
    g_frame ^= 1;
}

Pad read_pad() {
    static bool was_connected = true;
    static uint64_t gone_since = 0;
    Pad pad;
    OrbisPadData data{};
    const bool ok = g_pad >= 0 && scePadReadState(g_pad, &data) >= 0 && data.connected;
    const uint64_t now = now_ms();
    if (ok != was_connected) {
        was_connected = ok;
        LOG(ok ? "controller connected" : "controller disconnected");
        gone_since = ok ? 0 : now;
    }
    if (!ok) {
        // A controller that turned itself off may come back on a new handle.
        if (gone_since && now - gone_since >= 2000) {
            gone_since = now;
            if (g_pad >= 0) scePadClose(g_pad);
            int user = 0;
            if (sceUserServiceGetInitialUser(&user) >= 0) g_user = user;
            g_pad = scePadOpen(g_user, 0, 0, nullptr);
            g_light = 0xFFFFFFFF;
        }
        pad.connected = false;
        return pad;
    }
    // Bit 31 is set while the system intercepts input (PS button menu).
    pad.buttons = data.buttons & ~0x80000000u;
    pad.lx = (int)data.leftStick.x - 128;
    pad.ly = (int)data.leftStick.y - 128;
    pad.rx = (int)data.rightStick.x - 128;
    pad.ry = (int)data.rightStick.y - 128;
    return pad;
}

void set_light_bar(uint32_t rgb) {
    if (g_pad < 0 || rgb == g_light) return;
    g_light = rgb;
    OrbisPadColor color{(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 0};
    scePadSetLightBar(g_pad, &color);
}

int utc_offset_seconds() {
    // Read again every half minute: the user may change zone or summer time.
    static int offset = 0;
    static time_t read_at = 0;
    const time_t now = time(nullptr);
    if (!read_at || now - read_at >= 30 || now < read_at) {
        read_at = now;
        int32_t v = 0;
        offset = 0;
        if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_TIME_ZONE, &v) >= 0) offset = v * 60;
        if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_SUMMERTIME, &v) >= 0 && v) offset += 3600;
    }
    return offset;
}

bool clock_24h() {
    int32_t v = 1;
    // 0: 12-hour, 1: 24-hour.
    return sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_TIME_FORMAT, &v) < 0 || v == 1;
}

uint64_t now_ms() { return sceKernelGetProcessTime() / 1000; }
void sleep_ms(int ms) { usleep(ms * 1000); }

void request_redraw() { g_redraw = true; }
bool take_redraw() { return g_redraw.exchange(false); }

std::string data_root() { return g_data_root.empty() ? "/data/Librarian" : g_data_root; }

std::string system_language() {
    // ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG (1): 0 Japanese, 1 English (US),
    // 2 French, 3 Spanish, 4 German, 7 Portuguese, 17 Brazilian Portuguese,
    // 18 English (UK), 20 Latin American Spanish, 22 Canadian French.
    int32_t v = 1;
    if (sceSystemServiceParamGetInt(1, &v) < 0) v = 1;
    switch (v) {
        case 0: return "ja";
        case 2: case 22: return "fr";
        case 3: case 20: return "es";
        case 4: return "de";
        case 7: case 17: return "pt-BR";
        default: return "en";
    }
}

std::string device_secret() {
    static std::string secret;
    static bool tried = false;
    if (tried) return secret;
    tried = true;
    unsigned char id[16] = {0};
    const int32_t rc = sceKernelGetOpenPsIdForSystem(id);
    bool any = false;
    for (unsigned char b : id) any = any || b != 0;
    if (rc == 0 && any) {
        secret.assign((const char*)id, sizeof id);
        LOG("settings: protected with this console's ID");
    } else {
        LOG("settings: the console ID is not available (rc 0x%08x); settings are protected by this folder's key only",
            (unsigned)rc);
    }
    return secret;
}
bool enable_usb_access() {
    // Must run once on the main thread, before any worker threads start: the
    // credential and vnode changes apply to the whole process.
    // The sound port first, while the process still has its own identity
    // (system services may refuse the escaped one); it is kept for the whole
    // session.
    audio_open();
    log_usb_slots("in the sandbox");
    if (sceKernelIsInSandbox() == 0) {
        LOG("usb: already outside the sandbox");
        return true;
    }
    LOG("usb: leaving the sandbox with %s", sandbox::method());
    const bool out = sandbox::leave();
    if (out) log_usb_slots("outside the sandbox");
    // Still inside (GoldHEN's SDK call does not answer on some firmware, 14.00
    // for one): the drives are read and written through GoldHEN's own FTP
    // server on the console.
    if (!out) {
        lib::usb_ftp::enable("127.0.0.1:2121", "/mnt/usb");
        LOG("usb: the sandbox stays on; USB drives go through GoldHEN's FTP server at 127.0.0.1:2121");
    }
    // Whether the sound service still accepts this process (the port opened
    // above stays in use either way).
    const int probe = open_port();
    if (probe >= 0) sceAudioOutClose(probe);
    LOG("audio: a port %s %s (0x%08x)", probe >= 0 ? "opens" : "does not open",
        out ? "outside the sandbox" : "after the escape attempt", (unsigned)probe);
    return out;
}

// ---- crash log ------------------------------------------------------------------
// The kernel is FreeBSD: its signal numbers (SIGBUS 10, not Linux's 7), its
// struct sigaction (through libkernel's sigaction, not musl's) and its
// siginfo / ucontext layouts, from FreeBSD's <sys/signal.h> and
// <machine/ucontext.h> for amd64.
namespace {
struct FreeBsdSigaction {
    void (*handler)(int, void*, void*);
    int flags;
    uint32_t mask[4];
};
extern "C" int crash_sigaction(int sig, const FreeBsdSigaction* act, FreeBsdSigaction* old) __asm__("sigaction");
constexpr int kSaSigInfo = 0x40, kSaResetHand = 0x4;
char g_crash_path[256];

const char* signal_name(int sig) {
    switch (sig) {
        case 4: return "illegal instruction";
        case 6: return "abort";
        case 8: return "arithmetic error";
        case 10: return "bus error";
        case 11: return "segmentation fault";
        default: return "signal";
    }
}

void on_crash(int sig, void* info, void* context) {
    // siginfo: si_addr after six 32-bit fields; ucontext: a 16-byte signal
    // mask, then mcontext, whose mc_rip is its 21st 64-bit slot.
    const uintptr_t fault = info ? *(const uintptr_t*)((const char*)info + 24) : 0;
    const uintptr_t pc = context ? *(const uintptr_t*)((const char*)context + 16 + 20 * 8) : 0;
    lib::log_crash(g_crash_path, sig, signal_name(sig), fault, pc, (uintptr_t)&install_crash_handler);
    // The handler was reset (SA_RESETHAND): returning lets the signal end the
    // app as it would have.
}
}  // namespace

void install_crash_handler() {
    snprintf(g_crash_path, sizeof g_crash_path, "%s/crash.log", g_data_root.c_str());
    for (int sig : {4, 6, 8, 10, 11}) {
        FreeBsdSigaction act{};
        act.handler = on_crash;
        act.flags = kSaSigInfo | kSaResetHand;
        if (crash_sigaction(sig, &act, nullptr) != 0) LOG("crash handler: signal %d not caught", sig);
    }
}

// Outside the sandbox /app0 may not exist; the package is also mounted here.
std::string assets_root() {
    static std::string root;
    if (root.empty() || !lib::dir_exists(root)) {
        root = "/app0/assets";
        if (!lib::dir_exists(root)) root = "/mnt/sandbox/pfsmnt/LIBR00001-app0/assets";
    }
    return root;
}

std::vector<std::string> usb_mount_points() {
    std::vector<std::string> slots;
    for (int i = 0; i < 8; ++i) slots.push_back("/mnt/usb" + std::to_string(i));
    return slots;
}

// A shared folder anyone can copy books into over FTP.
std::vector<std::string> internal_document_dirs() { return {"/data/books"}; }

// ---- sound --------------------------------------------------------------------------------
// The main port at 48 kHz, 16-bit stereo. It is opened
// at startup inside the sandbox (see enable_usb_access) and kept open, in case
// the audio service refuses a process that has left it.

namespace {
int g_audio_port = -1;
bool g_audio_init = false;

// The main port belongs to the system user (0xFF), as in SDL2's PS4 driver and
// in the OpenOrbis sample: opening it for the
// signed-in user is refused with 0x809B0001.
int open_port() {
    int port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kAudioBlock,
                               kAudioRate, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (port < 0) {
        LOG("audio: sceAudioOutOpen (system user) failed 0x%08x; trying user %08x", (unsigned)port, g_user);
        port = sceAudioOutOpen(g_user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kAudioBlock, kAudioRate,
                               ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    }
    return port;
}
}  // namespace

bool audio_open() {
    if (g_audio_port >= 0) return true;
    if (!g_audio_init) {
        const int rc = sceAudioOutInit();
        // "Already initialised" (ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) is fine.
        if (rc < 0 && (uint32_t)rc != 0x8026000Eu) LOG("audio: sceAudioOutInit 0x%08x", (unsigned)rc);
        g_audio_init = true;
    }
    g_audio_port = open_port();
    if (g_audio_port < 0 && sandbox::lifted()) {
        // Out of the sandbox: open it with the app's own credentials for a
        // moment, then step out again.
        LOG("audio: sceAudioOutOpen failed 0x%08x outside the sandbox; retrying with the app's credentials",
            (unsigned)g_audio_port);
        sandbox::with_app_credentials([] { g_audio_port = open_port(); });
    }
    if (g_audio_port < 0) {
        LOG("audio: sceAudioOutOpen failed 0x%08x", (unsigned)g_audio_port);
        g_audio_port = -1;
        return false;
    }
    LOG("audio: output open (port %d, %s the sandbox)", g_audio_port, sandbox::lifted() ? "outside" : "inside");
    return true;
}

bool audio_output(const int16_t* block) {
    if (g_audio_port < 0) return false;
    const int rc = sceAudioOutOutput(g_audio_port, block);
    if (rc < 0) {
        LOG("audio: sceAudioOutOutput failed 0x%08x", (unsigned)rc);
        return false;
    }
    return true;
}

void audio_close() {
    // Kept open for the session (it may not open again outside the sandbox);
    // shutdown() closes it. Only let the queued block finish.
    if (g_audio_port >= 0) sceAudioOutOutput(g_audio_port, nullptr);
}

namespace {
void audio_release() {
    if (g_audio_port < 0) return;
    sceAudioOutOutput(g_audio_port, nullptr);
    sceAudioOutClose(g_audio_port);
    g_audio_port = -1;
    LOG("audio: output closed");
}
}  // namespace

void background_thread_priority() {
    // A larger number is a lower priority: 700 is the default, 767 the lowest.
    scePthreadSetprio(scePthreadSelf(), 760);
}

std::string memory_report() {
    size_t free_bytes = 0;
    return lib_flexible_memory_free(&free_bytes) >= 0 ? "free memory " + std::to_string(free_bytes >> 20) + " MB"
                                                       : "free memory unknown";
}

}  // namespace platform
