// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "services/secure_store.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/fs.h"
#include "core/log.h"
#include "platform/platform.h"

extern "C" {
#include "monocypher/monocypher.h"
}

namespace svc {

namespace {

constexpr char kMagic[8] = {'L', 'I', 'B', 'R', 'E', 'N', 'C', '1'};
constexpr size_t kNonce = 24, kMac = 16, kHeader = sizeof kMagic + kNonce + kMac;
// A constant of Librarian's own, so the key differs from anything else made
// from the same console ID.
constexpr char kContext[] = "Librarian settings key v1 - jw-notes";

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Bytes that differ every call: they make nonces unique (which is all a
// nonce needs) and the folder's salt unpredictable enough. No system random
// library is linked for this (one missing on the console would stop the app
// from starting); the clock, the CPU's cycle counter, addresses and a counter
// are hashed instead.
void fresh_bytes(uint8_t* out, size_t size) {
    static uint64_t counter = 0;
    struct {
        uint64_t counter, cycles;
        struct timespec real, mono;
        const void* stack;
        const void* heap;
    } seed;
    seed.counter = ++counter;
    uint32_t lo = 0, hi = 0;
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
#endif
    seed.cycles = (uint64_t)hi << 32 | lo;
    clock_gettime(CLOCK_REALTIME, &seed.real);
    clock_gettime(CLOCK_MONOTONIC, &seed.mono);
    seed.stack = &seed;
    void* heap = malloc(16);
    seed.heap = heap;
    free(heap);
    const std::string device = platform::device_secret();
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, (const uint8_t*)&seed, sizeof seed);
    crypto_blake2b_update(&ctx, (const uint8_t*)device.data(), device.size());
    uint8_t hash[64];
    crypto_blake2b_final(&ctx, hash);
    memcpy(out, hash, size < sizeof hash ? size : sizeof hash);
    crypto_wipe(hash, sizeof hash);
}

// The key: from the console's ID and the folder's salt (made once).
bool derive_key(uint8_t key[32]) {
    const std::string salt_path = lib::join_path(platform::data_root(), ".key");
    std::string salt;
    if (!lib::read_file(salt_path, salt, 4096) || salt.size() != 32) {
        uint8_t fresh[32];
        fresh_bytes(fresh, sizeof fresh);
        salt.assign((const char*)fresh, sizeof fresh);
        if (!lib::write_file_atomic(salt_path, salt)) {
            LOG("settings: could not keep the key salt");
            return false;
        }
    }
    const std::string device = platform::device_secret();
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, (const uint8_t*)kContext, sizeof kContext - 1);
    crypto_blake2b_update(&ctx, (const uint8_t*)device.data(), device.size());
    crypto_blake2b_update(&ctx, (const uint8_t*)salt.data(), salt.size());
    crypto_blake2b_final(&ctx, key);
    return true;
}

}  // namespace

bool is_secure(const std::string& data) { return data.size() >= kHeader && memcmp(data.data(), kMagic, sizeof kMagic) == 0; }

bool write_secure(const std::string& path, const std::string& plain) {
    pthread_mutex_lock(&g_lock);
    uint8_t key[32];
    const bool have_key = derive_key(key);
    pthread_mutex_unlock(&g_lock);
    if (!have_key) return false;
    std::string out(kHeader + plain.size(), '\0');
    uint8_t* p = (uint8_t*)&out[0];
    memcpy(p, kMagic, sizeof kMagic);
    uint8_t* nonce = p + sizeof kMagic;
    uint8_t* mac = nonce + kNonce;
    fresh_bytes(nonce, kNonce);
    // The magic is authenticated too.
    crypto_aead_lock(p + kHeader, mac, key, nonce, (const uint8_t*)kMagic, sizeof kMagic, (const uint8_t*)plain.data(),
                     plain.size());
    crypto_wipe(key, sizeof key);
    return lib::write_file_atomic(path, out);
}

SecureRead read_secure(const std::string& path, std::string& out) {
    out.clear();
    std::string data;
    if (!lib::read_file(path, data)) return SecureRead::Missing;
    if (!is_secure(data)) {
        out = std::move(data);
        return SecureRead::Plain;
    }
    pthread_mutex_lock(&g_lock);
    uint8_t key[32];
    const bool have_key = derive_key(key);
    pthread_mutex_unlock(&g_lock);
    if (!have_key) return SecureRead::Failed;
    const uint8_t* p = (const uint8_t*)data.data();
    out.resize(data.size() - kHeader);
    const int rc = crypto_aead_unlock((uint8_t*)&out[0], p + sizeof kMagic + kNonce, key, p + sizeof kMagic,
                                      (const uint8_t*)kMagic, sizeof kMagic, p + kHeader, out.size());
    crypto_wipe(key, sizeof key);
    if (rc != 0) {
        out.clear();
        return SecureRead::Failed;
    }
    return SecureRead::Decrypted;
}

}  // namespace svc
