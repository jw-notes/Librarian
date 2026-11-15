// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// USB drives through GoldHEN's FTP server: see usb_ftp.h.
//
// Measured on GoldHEN's server: ABOR
// and EPSV answer 502; REST with RETR reads from anywhere; closing a
// transfer's data connection early gets 426 and leaves the control connection
// usable; APPE appends. REST with STOR does NOT write at the offset: it empties
// the file, so it is never used. The server reports neither free space nor the
// drive's file system. LIST rows look like
//   "-rwxrwxrwx 1 -1 -1 14384367 Nov  9 2026 AAC-5.1.mkv"
// (the name, spaces and all, is what follows the eighth field).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1  // fopencookie
#endif
#include "core/usb_ftp.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>

#include "core/log.h"
#include "core/text.h"

// On the console the toolchain's MSG_NOSIGNAL (Linux's 0x4000) means something
// else to the FreeBSD kernel, so it is not passed there; the socket option and
// ignoring SIGPIPE stand in for it.
#if !defined(MSG_NOSIGNAL) || !defined(LIB_HOST)
#undef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace lib::usb_ftp {

namespace {

std::string g_address, g_prefix;
bool g_enabled = false;

int connect_to(const std::string& host, const std::string& port) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) return -1;
    int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd >= 0 && connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(found);
    // No SIGPIPE from this socket. The toolchain's headers are Linux's and
    // leave the option out; on the console's FreeBSD kernel it is 0x800.
#if defined(SO_NOSIGPIPE)
    if (fd >= 0) {
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    }
#elif !defined(LIB_HOST)
    if (fd >= 0) {
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, 0x800, &on, sizeof on);
    }
#endif
    return fd;
}

// Up to `wait_ms` of silence ends a read.
int read_some(int fd, char* into, int room, int wait_ms = 5000) {
    pollfd wait{};
    wait.fd = fd;
    wait.events = POLLIN;
    if (poll(&wait, 1, wait_ms) <= 0) return -1;
    return (int)recv(fd, into, room, 0);
}

bool send_all(int fd, const char* bytes, size_t count, int wait_ms) {
    while (count) {
        pollfd wait{};
        wait.fd = fd;
        wait.events = POLLOUT;
        if (poll(&wait, 1, wait_ms) <= 0) return false;
        const ssize_t sent = send(fd, bytes, std::min<size_t>(count, 64 * 1024), MSG_NOSIGNAL);
        if (sent <= 0) return false;
        bytes += sent;
        count -= (size_t)sent;
    }
    return true;
}

uint64_t hash_text(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h ? h : 1;
}

class Ftp {
public:
    ~Ftp() { shut(); }
    void shut() {
        if (control_ >= 0) close(control_);
        control_ = -1;
        pending_.clear();
    }
    bool connected() const { return control_ >= 0; }
    const std::string& last() const { return last_; }

    bool open() {
        const auto colon = g_address.find(':');
        if (colon == std::string::npos) return false;
        host_ = g_address.substr(0, colon);
        control_ = connect_to(host_, g_address.substr(colon + 1));
        if (control_ < 0) return false;
        if (reply() / 100 != 2) return fail();
        int user = command("USER anonymous");
        if (user == 331) user = command("PASS anonymous");
        if (user / 100 != 2) return fail();
        return command("TYPE I") / 100 == 2 || fail();
    }

    // A reply ends with the line that has its code and a space ("211-" goes on).
    int reply(int wait_ms = 5000) {
        for (;;) {
            auto end = pending_.find('\n');
            while (end == std::string::npos) {
                char buffer[1024];
                const int n = control_ < 0 ? -1 : read_some(control_, buffer, sizeof buffer, wait_ms);
                if (n <= 0) {
                    shut();
                    return -1;
                }
                pending_.append(buffer, n);
                end = pending_.find('\n');
            }
            last_ = pending_.substr(0, end);
            pending_.erase(0, end + 1);
            if (!last_.empty() && last_.back() == '\r') last_.pop_back();
            if (last_.size() >= 4 && isdigit((unsigned char)last_[0]) && isdigit((unsigned char)last_[1]) &&
                isdigit((unsigned char)last_[2]) && last_[3] == ' ')
                return atoi(last_.c_str());
        }
    }

    int command(const std::string& text, int wait_ms = 5000) {
        const std::string line = text + "\r\n";
        if (control_ < 0 || !send_all(control_, line.data(), line.size(), wait_ms)) {
            shut();
            return -1;
        }
        return reply(wait_ms);
    }

    // The data connection for the next transfer. The server names its address
    // on the network; on the console the loopback reaches it too.
    int passive() {
        if (command("PASV") != 227) return -1;
        const auto at = last_.find('(');
        int a, b, c, d, high, low;
        if (at == std::string::npos ||
            sscanf(last_.c_str() + at + 1, "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &high, &low) != 6)
            return -1;
        const std::string port = std::to_string(high * 256 + low);
        const int fd = connect_to(host_, port);
        if (fd >= 0) return fd;
        return connect_to(std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." +
                              std::to_string(d),
                          port);
    }

    int64_t size(const std::string& path) {
        if (command("SIZE " + path) != 213 || last_.size() < 5) return -1;
        return (int64_t)strtoull(last_.c_str() + 4, nullptr, 10);
    }

    bool list(const std::string& dir, std::vector<DirEntry>& out) {
        out.clear();
        if (command("CWD " + dir) / 100 != 2) return false;
        const int data = passive();
        if (data < 0) return false;
        const int started = command("LIST");
        if (started / 100 != 1 && started / 100 != 2) {
            close(data);
            return false;
        }
        std::string text;
        std::vector<char> buffer(16 * 1024);
        int n;
        while ((n = read_some(data, buffer.data(), (int)buffer.size())) > 0) text.append(buffer.data(), n);
        close(data);
        if (n < 0) return false;
        if (started / 100 == 1 && reply() / 100 != 2) return false;
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t pos = 0;
            std::string size, date;
            bool whole = true;
            for (int field = 0; field < 8; ++field) {
                while (pos < line.size() && line[pos] == ' ') ++pos;
                const auto stop = line.find(' ', pos);
                if (stop == std::string::npos) {
                    whole = false;
                    break;
                }
                if (field == 4) size = line.substr(pos, stop - pos);
                if (field >= 5) date += line.substr(pos, stop - pos) + " ";
                pos = stop;
            }
            if (!whole) continue;
            while (pos < line.size() && line[pos] == ' ') ++pos;
            DirEntry e;
            e.name = line.substr(pos);
            if (e.name.empty() || e.name == "." || e.name == "..") continue;
            e.is_dir = line[0] == 'd';
            e.size = e.is_dir ? 0 : strtoull(size.c_str(), nullptr, 10);
            // No file serial numbers over FTP: one from the name, size and date
            // lets the library keep what it knows about an unchanged file.
            e.inode = hash_text(e.name + "\n" + size + "\n" + date);
            out.push_back(std::move(e));
        }
        return true;
    }

private:
    bool fail() {
        shut();
        return false;
    }
    int control_ = -1;
    std::string host_, pending_, last_;
};

// ---- connections ---------------------------------------------------------------
// Each conversation needs a control connection; a few are kept open for the
// next one. How many clients GoldHEN's server takes at once is not known, so
// at most kMaxBusy are in use; others wait their turn.
constexpr int kMaxBusy = 3;
constexpr size_t kMaxIdle = 2;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_turn = PTHREAD_COND_INITIALIZER;
std::vector<std::unique_ptr<Ftp>> g_idle;
int g_busy = 0;
std::string g_note;
std::atomic<int> g_answering{0};

void note(const std::string& text) {
    pthread_mutex_lock(&g_lock);
    const bool changed = text != g_note;
    g_note = text;
    pthread_mutex_unlock(&g_lock);
    if (changed) LOG("%s", text.c_str());
}

std::unique_ptr<Ftp> acquire() {
    pthread_mutex_lock(&g_lock);
    while (g_busy >= kMaxBusy) pthread_cond_wait(&g_turn, &g_lock);
    ++g_busy;
    std::unique_ptr<Ftp> ftp;
    if (!g_idle.empty()) {
        ftp = std::move(g_idle.back());
        g_idle.pop_back();
    }
    pthread_mutex_unlock(&g_lock);
    // A kept connection may have been closed by the server meanwhile.
    if (ftp && ftp->command("NOOP", 2000) / 100 != 2) ftp.reset();
    if (!ftp) {
        ftp.reset(new (std::nothrow) Ftp);
        if (ftp && !ftp->open()) {
            ftp.reset();
            g_answering = -1;
            note("usb: GoldHEN's FTP server does not answer at " + g_address +
                 "; turn FTP on in GoldHEN's settings to see USB drives");
        } else if (ftp) {
            g_answering = 1;
            note("usb: reading USB drives through GoldHEN's FTP server at " + g_address);
        }
    }
    if (!ftp) {
        pthread_mutex_lock(&g_lock);
        --g_busy;
        pthread_cond_signal(&g_turn);
        pthread_mutex_unlock(&g_lock);
    }
    return ftp;
}

void release(std::unique_ptr<Ftp> ftp) {
    pthread_mutex_lock(&g_lock);
    --g_busy;
    if (ftp && ftp->connected() && g_idle.size() < kMaxIdle) g_idle.push_back(std::move(ftp));
    pthread_cond_signal(&g_turn);
    pthread_mutex_unlock(&g_lock);
    // (a connection not kept closes here, outside the lock)
}

// Runs `fn` on a connection; once more on a fresh one if the kept one failed.
template <typename Fn>
bool with_ftp(Fn fn) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto ftp = acquire();
        if (!ftp) return false;
        const bool ok = fn(*ftp);
        const bool lost = !ftp->connected();
        release(std::move(ftp));
        if (ok || !lost) return ok;
    }
    return false;
}

// ---- reading -------------------------------------------------------------------
// A file is read in blocks, each its own short transfer (REST, RETR, close
// early), so an open book does not hold a connection while it is being read.
constexpr uint64_t kBlock = 256 * 1024;
constexpr size_t kBlocksKept = 4;

struct Reader {
    std::string path;
    uint64_t total = 0, at = 0;
    struct Block {
        uint64_t offset = UINT64_MAX;
        std::string bytes;
        uint64_t used = 0;
    };
    Block blocks[kBlocksKept];
    uint64_t tick = 0;

    const Block* fetch(uint64_t offset) {
        for (Block& b : blocks)
            if (b.offset == offset) {
                b.used = ++tick;
                return &b;
            }
        Block* slot = &blocks[0];
        for (Block& b : blocks)
            if (b.used < slot->used) slot = &b;
        const uint64_t want = std::min(kBlock, total - offset);
        std::string bytes;
        const bool ok = with_ftp([&](Ftp& ftp) {
            bytes.clear();
            const int data = ftp.passive();
            if (data < 0) return false;
            if ((offset && ftp.command("REST " + std::to_string(offset)) != 350) ||
                ftp.command("RETR " + path) / 100 != 1) {
                close(data);
                return false;
            }
            bytes.resize(want);
            uint64_t got = 0;
            while (got < want) {
                const int n = read_some(data, &bytes[got], (int)std::min<uint64_t>(want - got, 64 * 1024), 15000);
                if (n <= 0) break;
                got += (uint64_t)n;
            }
            close(data);
            bytes.resize(got);
            // 226 when the whole file was sent, 426 when the transfer was cut
            // short on purpose; either leaves the conversation in step.
            const int end = ftp.reply(got == want ? 3000 : 1000);
            if (end < 0) ftp.shut();
            return got == want;
        });
        if (!ok) {
            LOG("usb: FTP read of %s at %llu failed", path.c_str(), (unsigned long long)offset);
            return nullptr;
        }
        slot->offset = offset;
        slot->bytes = std::move(bytes);
        slot->used = ++tick;
        return slot;
    }
};

ssize_t reader_read(void* cookie, char* into, size_t room) {
    Reader* r = static_cast<Reader*>(cookie);
    size_t done = 0;
    while (done < room && r->at < r->total) {
        const uint64_t base = r->at / kBlock * kBlock;
        const Reader::Block* b = r->fetch(base);
        if (!b) return done ? (ssize_t)done : -1;
        const uint64_t inside = r->at - base;
        if (inside >= b->bytes.size()) break;
        const size_t k = (size_t)std::min<uint64_t>(room - done, b->bytes.size() - inside);
        memcpy(into + done, b->bytes.data() + inside, k);
        done += k;
        r->at += k;
    }
    return (ssize_t)done;
}

int reader_seek(void* cookie, off_t* offset, int whence) {
    Reader* r = static_cast<Reader*>(cookie);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)r->at : (int64_t)r->total;
    const int64_t to = base + (int64_t)*offset;
    if (to < 0) {
        errno = EINVAL;
        return -1;
    }
    r->at = (uint64_t)to;
    *offset = (off_t)to;
    return 0;
}

int reader_close(void* cookie) {
    delete static_cast<Reader*>(cookie);
    return 0;
}

// ---- writing -------------------------------------------------------------------
struct Writer {
    std::unique_ptr<Ftp> ftp;
    int data = -1;
    bool failed = false;
};

ssize_t writer_write(void* cookie, const char* bytes, size_t count) {
    Writer* w = static_cast<Writer*>(cookie);
    if (w->failed || !send_all(w->data, bytes, count, 30000)) {
        w->failed = true;
        errno = EIO;
        return -1;
    }
    return (ssize_t)count;
}

int writer_close(void* cookie) {
    Writer* w = static_cast<Writer*>(cookie);
    close(w->data);
    // The server may still be writing to the drive: its reply can take a while.
    const int end = w->ftp->reply(60000);
    const bool ok = !w->failed && end == 226;
    if (!ok) LOG("usb: FTP upload not confirmed (%d %s)", end, w->ftp->last().c_str());
    release(std::move(w->ftp));
    delete w;
    return ok ? 0 : -1;
}

}  // namespace

void enable(const std::string& address, const std::string& prefix) {
    g_address = address;
    g_prefix = prefix;
    g_enabled = !address.empty() && !prefix.empty();
    // A write to a connection the other end has closed must fail, not end the app.
    if (g_enabled) signal(SIGPIPE, SIG_IGN);
}

bool enabled() { return g_enabled; }
int answering() { return g_answering; }
bool handles(const std::string& path) { return g_enabled && starts_with(path, g_prefix); }
std::string address() { return g_address; }

bool reachable() {
    return with_ftp([](Ftp&) { return true; });
}

bool list(const std::string& dir, std::vector<DirEntry>& out) {
    return with_ftp([&](Ftp& ftp) { return ftp.list(dir, out); });
}

bool is_dir(const std::string& path) {
    bool yes = false;
    with_ftp([&](Ftp& ftp) {
        const int r = ftp.command("CWD " + path);
        yes = r / 100 == 2;
        return r >= 0;
    });
    return yes;
}

uint64_t size(const std::string& path, bool* found) {
    int64_t n = -1;
    with_ftp([&](Ftp& ftp) {
        n = ftp.size(path);
        return ftp.connected();
    });
    if (found) *found = n >= 0;
    return n > 0 ? (uint64_t)n : 0;
}

bool read_all(const std::string& path, std::string& out, uint64_t max_bytes) {
    out.clear();
    return with_ftp([&](Ftp& ftp) {
        out.clear();
        const int data = ftp.passive();
        if (data < 0) return false;
        if (ftp.command("RETR " + path) / 100 != 1) {
            close(data);
            return false;
        }
        std::vector<char> buffer(64 * 1024);
        int n;
        bool too_big = false;
        while ((n = read_some(data, buffer.data(), (int)buffer.size(), 15000)) > 0) {
            out.append(buffer.data(), n);
            if (out.size() > max_bytes) {
                too_big = true;
                break;
            }
        }
        close(data);
        const int end = ftp.reply(too_big ? 1000 : 5000);
        if (too_big || n < 0 || end != 226) {
            out.clear();
            return false;
        }
        return true;
    });
}

FILE* open_write(const std::string& path, bool append) {
    auto ftp = acquire();
    if (!ftp) return nullptr;
    const int data = ftp->passive();
    if (data < 0 || ftp->command((append ? "APPE " : "STOR ") + path) / 100 != 1) {
        if (data >= 0) close(data);
        LOG("usb: FTP could not create %s (%s)", path.c_str(), ftp->last().c_str());
        release(std::move(ftp));
        return nullptr;
    }
    Writer* w = new (std::nothrow) Writer;
    if (!w) {
        close(data);
        ftp->shut();
        release(std::move(ftp));
        return nullptr;
    }
    w->ftp = std::move(ftp);
    w->data = data;
    cookie_io_functions_t io{};
    io.write = writer_write;
    io.close = writer_close;
    FILE* f = fopencookie(w, append ? "ab" : "wb", io);
    if (!f) writer_close(w);
    return f;
}

bool write_all(const std::string& path, const std::string& data) {
    FILE* f = open_write(path);
    if (!f) return false;
    const bool wrote = fwrite(data.data(), 1, data.size(), f) == data.size();
    return (fclose(f) == 0) && wrote;
}

FILE* open_read(const std::string& path) {
    bool found = false;
    const uint64_t total = size(path, &found);
    if (!found) return nullptr;
    Reader* r = new (std::nothrow) Reader;
    if (!r) return nullptr;
    r->path = path;
    r->total = total;
    cookie_io_functions_t io{};
    io.read = reader_read;
    io.seek = reader_seek;
    io.close = reader_close;
    FILE* f = fopencookie(r, "rb", io);
    if (!f) delete r;
    return f;
}

bool make_dir(const std::string& path) {
    return with_ftp([&](Ftp& ftp) { return ftp.command("MKD " + path) / 100 == 2; });
}

bool remove_file(const std::string& path) {
    return with_ftp([&](Ftp& ftp) { return ftp.command("DELE " + path) / 100 == 2; });
}

bool remove_dir(const std::string& path) {
    return with_ftp([&](Ftp& ftp) { return ftp.command("RMD " + path) / 100 == 2; });
}

bool rename(const std::string& from, const std::string& to) {
    return with_ftp([&](Ftp& ftp) {
        if (ftp.size(to) >= 0) ftp.command("DELE " + to);
        return ftp.command("RNFR " + from) == 350 && ftp.command("RNTO " + to) / 100 == 2;
    });
}

}  // namespace lib::usb_ftp
