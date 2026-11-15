// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "core/worker.h"

#include <pthread.h>

#include <deque>
#include <map>
#include <vector>

#include "platform/platform.h"

namespace lib {

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
// Highest priority first; FIFO within a priority.
std::map<int, std::deque<std::function<void()>>, std::greater<int>> g_jobs;
std::vector<std::function<void()>> g_done;
std::vector<pthread_t> g_threads;
bool g_stopping = false;
int g_running = 0;

void* worker_main(void*) {
    platform::background_thread_priority();
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_stopping && g_jobs.empty()) pthread_cond_wait(&g_wake, &g_lock);
        if (g_stopping) {
            pthread_mutex_unlock(&g_lock);
            return nullptr;
        }
        auto queue = g_jobs.begin();
        std::function<void()> job = std::move(queue->second.front());
        queue->second.pop_front();
        if (queue->second.empty()) g_jobs.erase(queue);
        ++g_running;
        pthread_mutex_unlock(&g_lock);

        job();

        pthread_mutex_lock(&g_lock);
        --g_running;
        pthread_mutex_unlock(&g_lock);
        platform::request_redraw();
    }
}

}  // namespace

void workers_start(int count) {
    // Startable again after workers_stop (the host tests do both, more than once).
    pthread_mutex_lock(&g_lock);
    g_stopping = false;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < count; ++i) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // Layout and image decoding recurse a little; the default stack on the
        // console is small.
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        if (pthread_create(&thread, &attr, worker_main, nullptr) == 0) g_threads.push_back(thread);
        pthread_attr_destroy(&attr);
    }
}

void workers_stop() {
    pthread_mutex_lock(&g_lock);
    g_stopping = true;
    g_jobs.clear();
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    for (pthread_t t : g_threads) pthread_join(t, nullptr);
    g_threads.clear();
}

void run_async(std::function<void()> job, int priority) {
    pthread_mutex_lock(&g_lock);
    g_jobs[priority].push_back(std::move(job));
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

void post(std::function<void()> fn) {
    pthread_mutex_lock(&g_lock);
    g_done.push_back(std::move(fn));
    pthread_mutex_unlock(&g_lock);
    platform::request_redraw();
}

bool run_completions() {
    std::vector<std::function<void()>> ready;
    pthread_mutex_lock(&g_lock);
    ready.swap(g_done);
    pthread_mutex_unlock(&g_lock);
    for (auto& fn : ready) fn();
    return !ready.empty();
}

int jobs_in_flight() {
    pthread_mutex_lock(&g_lock);
    int n = g_running;
    for (const auto& [priority, queue] : g_jobs) n += (int)queue.size();
    pthread_mutex_unlock(&g_lock);
    return n;
}

}  // namespace lib
