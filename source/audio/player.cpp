// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "audio/player.h"

#include <pthread.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <memory>

#include "audio/decoder.h"
#include "audio/dsp.h"
#include "core/log.h"
#include "platform/platform.h"

namespace audio {

namespace {

constexpr size_t kRingFrames = kOutputRate * 8;
constexpr size_t kStartFrames = kOutputRate / 2;  // buffered before sound starts
constexpr size_t kRefillFrames = kOutputRate;      // after running dry
constexpr size_t kChunkRoom = kOutputRate;         // room the engine wants before decoding more

// Which output frame plays which moment of the book.
struct Marker {
    uint64_t frame;
    double time, speed;
};

std::shared_ptr<Decoder> open_track(const Track& t, const HeaderFn& headers, std::string& error) {
    std::unique_ptr<Decoder> d;
    if (t.kind == Track::Hls) {
        d = make_hls_decoder(t.location, headers);
    } else {
        std::shared_ptr<ByteSource> source = t.kind == Track::File
                                                 ? open_file_source(t.location, error, t.offset, t.slice_size)
                                                 : open_http_source(t.location, t.size, headers);
        if (!source) return nullptr;
        d = make_decoder(source, t.mime);
        if (!d) {
            error = "Librarian cannot play this audio format (" + (t.mime.empty() ? std::string("unknown") : t.mime) + ").";
            return nullptr;
        }
    }
    d->set_duration_hint(t.clip_end > 0 ? 0 : t.duration);
    if (!d->open(error)) return nullptr;
    if (t.clip_end > t.clip_begin) d = make_clip_decoder(std::move(d), t.clip_begin, t.clip_end);
    LOG("audio: %s, %d Hz, %d channels, %.0f s, %s", d->name(), d->rate(), d->channels(), d->duration(),
        t.kind == Track::File ? "file" : t.kind == Track::Http ? "streamed" : "transcoded stream");
    return std::shared_ptr<Decoder>(d.release());
}

int16_t to_s16(float v) {
    v *= 32767.f;
    return (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
}

}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::Loading: return "loading";
        case State::Buffering: return "buffering";
        case State::Playing: return "playing";
        case State::Paused: return "paused";
        case State::Ended: return "ended";
        case State::Error: return "error";
    }
    return "?";
}

struct Player::Impl {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wake = PTHREAD_COND_INITIALIZER;  // the engine has something to do
    pthread_t engine_thread{}, output_thread{};
    bool threads = false, quit = false;

    // What the screens asked for. `epoch` changes with every open, seek, speed
    // change and close; `book` with every open.
    bool loaded = false, paused = true, loading = false, eof = false, prebuffer = true;
    uint64_t epoch = 0, book = 0;
    double target = 0, speed = 1, position = 0, duration = 0;
    std::string error;
    std::vector<Track> tracks;
    HeaderFn headers;
    std::shared_ptr<Decoder> active;  // so a blocked read can be cancelled

    // The ring of 48 kHz stereo frames; head/tail count frames since the start.
    std::vector<int16_t> ring = std::vector<int16_t>(kRingFrames * 2);
    uint64_t head = 0, tail = 0;
    std::deque<Marker> markers;

    void start_threads() {
        if (threads) return;
        threads = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        pthread_create(&engine_thread, &attr, [](void* p) -> void* { ((Impl*)p)->engine(); return nullptr; }, this);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_create(&output_thread, &attr, [](void* p) -> void* { ((Impl*)p)->output(); return nullptr; }, this);
        pthread_attr_destroy(&attr);
    }

    // Called with the lock held.
    void restart(double at) {
        target = std::max(0.0, duration > 0 ? std::min(at, duration) : at);
        position = target;
        ++epoch;
        loading = true;
        eof = false;
        error.clear();
        pthread_cond_signal(&wake);
    }

    // ---- the engine thread ---------------------------------------------------------------

    void engine() {
        uint64_t seen_epoch = ~0ull, seen_book = ~0ull, pushed = 0;
        std::vector<Track> list;
        HeaderFn hdrs;
        std::shared_ptr<Decoder> dec;
        int ti = -1, retries = 0;
        Resampler rs;
        TimeStretch ts;
        double anchor = 0, skip_until = -1, sp = 1;
        bool finished = true;
        std::vector<float> pcm, stereo, resampled, out;

        auto fail = [&](const std::string& message) {
            pthread_mutex_lock(&lock);
            if (seen_epoch == epoch) {
                error = message.empty() ? "The audio could not be played." : message;
                loading = false;
                LOG("audio: %s", error.c_str());
            }
            pthread_mutex_unlock(&lock);
            finished = true;
        };
        auto use = [&](std::shared_ptr<Decoder> d) {
            pthread_mutex_lock(&lock);
            active = d;
            pthread_mutex_unlock(&lock);
            dec = std::move(d);
        };

        for (;;) {
            pthread_mutex_lock(&lock);
            while (!quit && seen_epoch == epoch &&
                   (!loaded || finished || kRingFrames - (size_t)(tail - head) < kChunkRoom + 4096))
                pthread_cond_wait(&wake, &lock);
            if (quit) {
                pthread_mutex_unlock(&lock);
                break;
            }
            if (seen_epoch != epoch) {
                // Open, seek, speed or close: start again from `target`.
                seen_epoch = epoch;
                head = tail = 0;
                markers.clear();
                prebuffer = true;
                const bool new_book = seen_book != book;
                seen_book = book;
                if (new_book) {
                    list = tracks;
                    hdrs = headers;
                }
                const double at = target;
                sp = speed;
                const bool have = loaded;
                pthread_mutex_unlock(&lock);
                if (!have || list.empty()) {
                    use(nullptr);
                    ti = -1;
                    finished = true;
                    continue;
                }
                int want = 0;
                while (want + 1 < (int)list.size() && list[want + 1].start <= at) ++want;
                if (new_book || want != ti || !dec) {
                    use(nullptr);
                    std::string err;
                    auto d = open_track(list[want], hdrs, err);
                    if (!d) {
                        ti = -1;
                        fail(err);
                        continue;
                    }
                    use(d);
                    ti = want;
                }
                const double local = std::max(0.0, at - list[ti].start);
                dec->seek(local);
                skip_until = local;
                rs.reset();
                rs.set_rate(dec->rate());
                ts.reset();
                ts.set_speed(sp);
                anchor = at;
                pushed = 0;
                retries = 0;
                finished = false;
                pthread_mutex_lock(&lock);
                if (seen_epoch == epoch) loading = false;
                pthread_mutex_unlock(&lock);
                continue;
            }
            pthread_mutex_unlock(&lock);

            // Decode a little and pass it on.
            pcm.clear();
            const double before = dec->position();
            const int n = dec->decode(pcm);
            if (n < 0) {
                // A stream that failed (the network) is reopened where it was.
                if (list[ti].kind != Track::File && retries < 3) {
                    ++retries;
                    LOG("audio: %s; reopening (attempt %d)", dec->error().c_str(), retries);
                    platform::sleep_ms(1000 * retries);
                    std::string err;
                    auto d = open_track(list[ti], hdrs, err);
                    if (d) {
                        d->seek(before);
                        skip_until = before;
                        use(d);
                    }
                    continue;
                }
                fail(dec->error());
                continue;
            }
            if (n == 0) {
                if (ti + 1 < (int)list.size()) {
                    // The next file of the book, without a gap.
                    std::string err;
                    auto d = open_track(list[++ti], hdrs, err);
                    if (!d) {
                        fail(err);
                        continue;
                    }
                    d->seek(0);
                    use(d);
                    skip_until = -1;
                    // Times are taken from the file's place in the book.
                    anchor = list[ti].start - (double)pushed / kOutputRate;
                    continue;
                }
                ts.finish();
                out.clear();
                const double mark = anchor + ts.input_position() / kOutputRate;
                ts.pull(out);
                push(out, mark, sp, seen_epoch);
                pthread_mutex_lock(&lock);
                if (seen_epoch == epoch) eof = true;
                pthread_mutex_unlock(&lock);
                finished = true;
                continue;
            }
            retries = 0;
            const int channels = std::max(1, dec->channels()), rate = std::max(1, dec->rate());
            int skip = 0;
            if (skip_until >= 0) {
                skip = std::max(0, std::min(n, (int)((skip_until - before) * rate + 0.5)));
                if (skip < n) skip_until = -1;
            }
            if (skip >= n) continue;
            stereo.clear();
            to_stereo(pcm.data() + (size_t)skip * channels, (size_t)(n - skip), channels, stereo);
            rs.set_rate(rate);
            resampled.clear();
            rs.process(stereo.data(), stereo.size() / 2, resampled);
            ts.push(resampled.data(), resampled.size() / 2);
            pushed += resampled.size() / 2;
            const double mark = anchor + ts.input_position() / kOutputRate;
            out.clear();
            ts.pull(out);
            push(out, mark, sp, seen_epoch);
        }
        use(nullptr);
    }

    void push(const std::vector<float>& frames, double time, double sp, uint64_t for_epoch) {
        const size_t n = frames.size() / 2;
        if (!n) return;
        pthread_mutex_lock(&lock);
        if (for_epoch == epoch) {
            const size_t room = kRingFrames - (size_t)(tail - head), count = std::min(n, room);
            markers.push_back({tail, time, sp});
            for (size_t i = 0; i < count; ++i) {
                const size_t at = (size_t)((tail + i) % kRingFrames) * 2;
                ring[at] = to_s16(frames[2 * i]);
                ring[at + 1] = to_s16(frames[2 * i + 1]);
            }
            tail += count;
        }
        pthread_mutex_unlock(&lock);
    }

    // ---- the output thread ---------------------------------------------------------------

    void output() {
        std::vector<int16_t> block((size_t)platform::kAudioBlock * 2);
        bool open = false, refill = false;
        int failures = 0;
        for (;;) {
            pthread_mutex_lock(&lock);
            if (quit) {
                pthread_mutex_unlock(&lock);
                break;
            }
            const bool run = loaded && !paused && !loading && error.empty();
            if (!run) {
                const bool release = !loaded && open;
                pthread_mutex_unlock(&lock);
                if (release) {
                    platform::audio_close();
                    open = false;
                }
                platform::sleep_ms(10);
                continue;
            }
            const size_t avail = (size_t)(tail - head);
            if (prebuffer) {
                if (avail >= (refill ? kRefillFrames : kStartFrames) || (eof && avail)) {
                    prebuffer = false;
                } else {
                    pthread_mutex_unlock(&lock);
                    platform::sleep_ms(10);
                    continue;
                }
            }
            // Less than a block before the end would mean a block padded with
            // silence: a gap. Only the very end of the book plays that way.
            if (avail == 0 || (avail < (size_t)platform::kAudioBlock && !eof)) {
                if (!eof) {
                    // Ran dry (a slow network): wait for a second of sound.
                    prebuffer = refill = true;
                    LOG("audio: buffer ran dry at %.1f s", position);
                }
                pthread_mutex_unlock(&lock);
                platform::sleep_ms(10);
                continue;
            }
            const size_t n = std::min(avail, (size_t)platform::kAudioBlock);
            for (size_t i = 0; i < n; ++i) {
                const size_t at = (size_t)((head + i) % kRingFrames) * 2;
                block[2 * i] = ring[at];
                block[2 * i + 1] = ring[at + 1];
            }
            std::fill(block.begin() + 2 * n, block.end(), (int16_t)0);
            while (markers.size() > 1 && markers[1].frame <= head) markers.pop_front();
            if (!markers.empty()) {
                const Marker& m = markers.front();
                position = m.time + (double)(head - m.frame) * m.speed / kOutputRate;
            }
            head += n;
            refill = false;
            pthread_cond_signal(&wake);
            pthread_mutex_unlock(&lock);

            if (!open && !(open = platform::audio_open())) {
                pthread_mutex_lock(&lock);
                error = "The console's sound output could not be opened.";
                pthread_mutex_unlock(&lock);
                continue;
            }
            if (platform::audio_output(block.data())) {
                failures = 0;
            } else if (++failures > 20) {
                pthread_mutex_lock(&lock);
                error = "The console's sound output stopped working.";
                pthread_mutex_unlock(&lock);
            } else {
                platform::sleep_ms(20);
            }
        }
        if (open) platform::audio_close();
    }
};

Player::Player() : impl_(new Impl) {}
Player::~Player() {
    shutdown();
    delete impl_;
}

Player& player() {
    static Player instance;
    return instance;
}

void Player::open(std::vector<Track> tracks, HeaderFn headers, double at, bool playing, double speed) {
    Impl& m = *impl_;
    pthread_mutex_lock(&m.lock);
    m.start_threads();
    if (m.active) m.active->cancel();
    m.duration = tracks.empty() ? 0 : tracks.back().start + tracks.back().duration;
    m.tracks = std::move(tracks);
    m.headers = std::move(headers);
    m.loaded = true;
    m.paused = !playing;
    m.speed = std::max(0.5, std::min(3.0, speed));
    ++m.book;
    m.restart(at);
    pthread_mutex_unlock(&m.lock);
}

void Player::play() {
    pthread_mutex_lock(&impl_->lock);
    impl_->paused = false;
    pthread_mutex_unlock(&impl_->lock);
}

void Player::pause() {
    pthread_mutex_lock(&impl_->lock);
    impl_->paused = true;
    pthread_mutex_unlock(&impl_->lock);
}

void Player::seek(double at) {
    pthread_mutex_lock(&impl_->lock);
    if (impl_->loaded) impl_->restart(at);
    pthread_mutex_unlock(&impl_->lock);
}

void Player::set_speed(double speed) {
    Impl& m = *impl_;
    pthread_mutex_lock(&m.lock);
    speed = std::max(0.5, std::min(3.0, speed));
    if (speed != m.speed) {
        m.speed = speed;
        // What is buffered was made at the old speed: start again from here.
        if (m.loaded) m.restart(m.position);
    }
    pthread_mutex_unlock(&m.lock);
}

void Player::close() {
    Impl& m = *impl_;
    pthread_mutex_lock(&m.lock);
    if (m.active) m.active->cancel();
    m.loaded = false;
    m.paused = true;
    m.tracks.clear();
    m.headers = nullptr;
    ++m.book;
    ++m.epoch;
    m.error.clear();
    pthread_cond_signal(&m.wake);
    pthread_mutex_unlock(&m.lock);
}

void Player::shutdown() {
    Impl& m = *impl_;
    pthread_mutex_lock(&m.lock);
    const bool running = m.threads;
    m.quit = true;
    if (m.active) m.active->cancel();
    pthread_cond_signal(&m.wake);
    pthread_mutex_unlock(&m.lock);
    if (running) {
        pthread_join(m.engine_thread, nullptr);
        pthread_join(m.output_thread, nullptr);
        m.threads = false;
    }
}

bool Player::loaded() const {
    pthread_mutex_lock(&impl_->lock);
    const bool v = impl_->loaded;
    pthread_mutex_unlock(&impl_->lock);
    return v;
}

State Player::state() const {
    Impl& m = *impl_;
    pthread_mutex_lock(&m.lock);
    State s = State::Playing;
    if (!m.loaded) s = State::Idle;
    else if (!m.error.empty()) s = State::Error;
    else if (m.loading) s = State::Loading;
    else if (m.eof && m.head == m.tail) s = State::Ended;
    else if (m.paused) s = State::Paused;
    else if (m.prebuffer) s = State::Buffering;
    pthread_mutex_unlock(&m.lock);
    return s;
}

double Player::position() const {
    pthread_mutex_lock(&impl_->lock);
    const double v = impl_->position;
    pthread_mutex_unlock(&impl_->lock);
    return v;
}

double Player::duration() const {
    pthread_mutex_lock(&impl_->lock);
    const double v = impl_->duration;
    pthread_mutex_unlock(&impl_->lock);
    return v;
}

double Player::speed() const {
    pthread_mutex_lock(&impl_->lock);
    const double v = impl_->speed;
    pthread_mutex_unlock(&impl_->lock);
    return v;
}

std::string Player::error() const {
    pthread_mutex_lock(&impl_->lock);
    const std::string v = impl_->error;
    pthread_mutex_unlock(&impl_->lock);
    return v;
}

double Player::buffered() const {
    pthread_mutex_lock(&impl_->lock);
    const double v = (double)(impl_->tail - impl_->head) / kOutputRate;
    pthread_mutex_unlock(&impl_->lock);
    return v;
}

}  // namespace audio
