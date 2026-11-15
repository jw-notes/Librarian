// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "core/log.h"

#include <atomic>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform/platform.h"

namespace lib {

static FILE* g_log = nullptr;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

// The last lines logged, for crash.log: a fixed buffer, so the crash handler
// needs no allocation to write it.
constexpr int kRecentLines = 40, kRecentWidth = 240;
static char g_recent[kRecentLines][kRecentWidth];
static int g_recent_next = 0;

void log_open(const std::string& path) {
    pthread_mutex_lock(&g_log_lock);
    if (g_log) fclose(g_log);
    const std::string previous = path.substr(0, path.rfind('.')) + ".prev.log";
    rename(path.c_str(), previous.c_str());
    g_log = fopen(path.c_str(), "w");
    pthread_mutex_unlock(&g_log_lock);
}

namespace {
std::atomic<bool> g_debug{false};
}

void log_set_debug(bool on) { g_debug = on; }
bool log_debug() { return g_debug; }

void log_line(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);

    const unsigned long long ms = platform::now_ms();
    pthread_mutex_lock(&g_log_lock);
    if (g_log) {
        fprintf(g_log, "[%7llu.%03llu] %s\n", ms / 1000, ms % 1000, message);
        fflush(g_log);
    }
    snprintf(g_recent[g_recent_next], kRecentWidth, "[%7llu.%03llu] %.200s", ms / 1000, ms % 1000, message);
    g_recent_next = (g_recent_next + 1) % kRecentLines;
#ifdef LIB_HOST
    fprintf(stderr, "[%7llu.%03llu] %s\n", ms / 1000, ms % 1000, message);
#endif
    pthread_mutex_unlock(&g_log_lock);
}

namespace {

void put(int fd, const char* text) {
    size_t left = strlen(text);
    while (left) {
        const ssize_t n = write(fd, text, left);
        if (n <= 0) return;
        text += n;
        left -= (size_t)n;
    }
}

// Numbers without printf, which is not safe in a signal handler.
void put_hex(int fd, uintptr_t value) {
    char out[19] = "0x";
    for (int i = 0; i < 16; ++i) out[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    out[18] = 0;
    put(fd, out);
}

void put_number(int fd, unsigned long value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < 22);
    char text[24];
    for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
    text[n] = 0;
    put(fd, text);
}

void put_report(int fd, int signal, const char* name, uintptr_t fault, uintptr_t pc, uintptr_t main_at) {
    put(fd, "Librarian crashed: signal ");
    put_number(fd, (unsigned long)signal);
    put(fd, " (");
    put(fd, name);
    put(fd, ")\nfault address ");
    put_hex(fd, fault);
    put(fd, "\ninstruction   ");
    put_hex(fd, pc);
    put(fd, "\nreference at  ");
    put_hex(fd, main_at);
    put(fd, "\n(reference = platform::install_crash_handler; the instruction's place in the\n"
            " program = instruction - reference + that function's address in the ELF)\n");
}

}  // namespace

void log_crash(const char* crash_path, int signal, const char* name, uintptr_t fault, uintptr_t pc,
               uintptr_t main_at) {
    const unsigned long long ms = platform::now_ms();
    // The log lock may be held by the thread that crashed: not waited for.
    const bool locked = pthread_mutex_trylock(&g_log_lock) == 0;
    const int fd = open(crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        put_report(fd, signal, name, fault, pc, main_at);
        put(fd, "time          ");
        put_number(fd, (unsigned long)(ms / 1000));
        put(fd, " s after start\n\nLast log lines:\n");
        for (int i = 0; i < kRecentLines; ++i) {
            const char* line = g_recent[(g_recent_next + i) % kRecentLines];
            if (!line[0]) continue;
            put(fd, line);
            put(fd, "\n");
        }
        close(fd);
    }
    if (g_log) {
        fflush(g_log);
        const int log_fd = fileno(g_log);
        if (log_fd >= 0) put_report(log_fd, signal, name, fault, pc, main_at);
    }
    if (locked) pthread_mutex_unlock(&g_log_lock);
}

bool log_previous_crash(const std::string& crash_path) {
    FILE* f = fopen(crash_path.c_str(), "rb");
    if (!f) return false;
    char line[256];
    int n = 0;
    while (n < 4 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        log_line("previous run: %s", line);
        ++n;
    }
    fclose(f);
    const std::string previous = crash_path.substr(0, crash_path.rfind('.')) + ".prev.log";
    rename(crash_path.c_str(), previous.c_str());
    return true;
}

}  // namespace lib
