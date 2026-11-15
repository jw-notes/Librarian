// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Background work. Network requests and decoding run on worker threads; their
// results come back to the UI thread through `post`, and are run there by
// `run_completions` once per frame. Screens hold a CancelToken so a result
// for a screen that has been closed is dropped instead of applied.
#include <atomic>
#include <functional>
#include <memory>

namespace lib {

using CancelToken = std::shared_ptr<std::atomic<bool>>;
inline CancelToken make_token() { return std::make_shared<std::atomic<bool>>(false); }

void workers_start(int count);
void workers_stop();

// Runs `job` on a worker thread. Jobs at higher priority run first (artwork
// uses a low priority, so page content is never stuck behind thumbnails).
void run_async(std::function<void()> job, int priority = 1);
// Queues `fn` to run on the UI thread.
void post(std::function<void()> fn);
// Runs queued UI-thread functions; returns true if any ran.
bool run_completions();
// Number of jobs queued or running, for the busy indicator.
int jobs_in_flight();

// The usual pattern: compute T on a worker, deliver it to the UI thread
// unless the token was cancelled first.
template <typename T>
void async(CancelToken token, std::function<T()> work, std::function<void(T&)> done, int priority = 1) {
    run_async(
        [token, work = std::move(work), done = std::move(done)]() mutable {
            if (token && *token) return;
            auto result = std::make_shared<T>(work());
            post([token, result, done = std::move(done)]() {
                if (!token || !*token) done(*result);
            });
        },
        priority);
}

}  // namespace lib
