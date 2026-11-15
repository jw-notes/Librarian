// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/app.h"
#include "ui/i18n.h"

#include "core/log.h"
#include "core/worker.h"
#include "gfx/font.h"
#include "platform/platform.h"
#include "services/library.h"
#include "services/listening.h"
#include "services/updater.h"
#include "services/local_library.h"
#include "services/zim_shelf.h"
#include "ui/widgets.h"

#ifdef LIB_HOST
#include "platform/host.h"
#endif

namespace ui {

using namespace platform;

App& app() {
    static App instance;
    return instance;
}

void App::push(std::unique_ptr<Screen> screen) { pending_push_.push_back(std::move(screen)); }
void App::pop() { ++pending_pops_; }
void App::switch_tab(Tab tab) {
    // A hidden tab (a server's with no server set up) opens the first one shown.
    for (int i = 0; !tab_visible(tab) && i < (int)Tab::Count; ++i) tab = (Tab)i;
    pending_root_ = make_tab_screen(tab);
}

void App::toast(const std::string& message) {
    toast_ = T(message);
    toast_until_ = now_ms() + 2600;
    request_redraw();
}

Screen* App::below(const Screen* s) {
    for (size_t i = 1; i < stack_.size(); ++i)
        if (stack_[i].get() == s) return stack_[i - 1].get();
    return nullptr;
}

void App::apply_pending() {
    bool changed = false;
    if (pending_root_) {
        stack_.clear();
        stack_.push_back(std::move(pending_root_));
        pending_pops_ = 0;
        changed = true;
    }
    for (; pending_pops_ > 0 && !stack_.empty(); --pending_pops_) {
        stack_.pop_back();
        changed = true;
        if (stack_.empty()) break;
    }
    pending_pops_ = 0;
    for (auto& s : pending_push_) {
        stack_.push_back(std::move(s));
        changed = true;
    }
    const bool pushed = !pending_push_.empty();
    pending_push_.clear();
    if (changed) {
        input_.reset();
        if (!pushed && !stack_.empty()) stack_.back()->resumed();
        request_redraw();
    }
    if (stack_.empty()) stack_.push_back(make_tab_screen(Tab::Home));
}

void App::draw() {
    gfx::Canvas c(canvas(), frame_width(), frame_height(), kWidth, kHeight);
    // Draw from the topmost non-overlay screen upwards.
    size_t first = stack_.size() - 1;
    while (first > 0 && stack_[first]->overlay()) --first;
    Screen* base = stack_[first].get();
    if (!base->full_screen()) {
        draw_background(c);
        draw_header(c, base->tab());
    }
    base->draw(c);
    // (Not under the now-playing strip, which covers that corner.)
    if (!base->full_screen()) draw_now_playing(c);
    if (!base->full_screen() && first == stack_.size() - 1) draw_footer(c, base->hints());
    for (size_t i = first + 1; i < stack_.size(); ++i) stack_[i]->draw(c);
    if (stack_.size() - 1 > first) draw_footer(c, stack_.back()->hints());

    if (!toast_.empty() && now_ms() < toast_until_) {
        const Theme& t = theme();
        const int px = kBodyPx, w = gfx::text_width(gfx::Face::Sans, px, toast_) + 72;
        const gfx::Rect r{(kWidth - w) / 2, kFooterTop - 110, w, 72};
        c.fill_rounded(r, 36, gfx::with_alpha(t.surface_hi, 245));
        gfx::draw_text(c, gfx::Face::Sans, px, r.x + 36, r.y + (72 - gfx::font_metrics(gfx::Face::Sans, px).line_height) / 2,
                       toast_, t.text);
    }
}

void App::run() {
    switch_tab(Tab::Home);
    apply_pending();
    uint64_t last_save = now_ms();
    bool toast_visible = false;
    while (!quit_) {
#ifdef LIB_HOST
        if (host::quit_requested()) break;
#endif
        const Input in = input_.read();
        const bool worked = lib::run_completions();
        // USB drives coming and going.
        svc::local_library().tick();
        // ZIM files the local library found anywhere on the drives.
        svc::zim_shelf().tick();
        // Audiobook progress, its syncing and the sleep timer.
        svc::listening().tick();
        // A newer Librarian: said once, wherever the user is.
        static bool update_told = false;
        if (!update_told && svc::updater::state() == svc::updater::State::Available) {
            update_told = true;
            toast("Librarian " + svc::updater::available_version() + " is available: Settings \xE2\x80\xBA System");
        }
        Screen* top = stack_.back().get();

        // Tabs: L1/R1 on any tab's root screen (hidden tabs are skipped).
        if (stack_.size() == 1 && top->tab() != Tab::None && (in.press(kL1) || in.press(kR1))) {
            const int n = (int)Tab::Count, step = in.press(kR1) ? 1 : n - 1;
            int next = ((int)top->tab() + step) % n;
            while (!tab_visible((Tab)next)) next = (next + step) % n;
            switch_tab((Tab)next);
        } else if (in.press(kTouchPad) && svc::listening().active() && !top->full_screen() && !top->overlay()) {
            // The player, from anywhere, while something plays.
            open_player();
        } else {
            top->update(in);
        }
        apply_pending();

        const bool toast_now = !toast_.empty() && now_ms() < toast_until_;
        // A playing audiobook's times move: redraw twice a second, not every frame.
        bool clock = false;
        if (svc::listening().active() && now_ms() - last_tick_ >= 500) {
            last_tick_ = now_ms();
            clock = true;
        }
        const bool dirty = take_redraw() || worked || in.pressed || in.repeated || in.held || in.rx || in.ry || in.lx || in.ly ||
                           stack_.back()->animating() || toast_now != toast_visible || lib::jobs_in_flight() > 0 || clock;
        toast_visible = toast_now;
        if (dirty) {
            draw();
            present();
        } else {
            sleep_ms(16);
        }
        if (now_ms() - last_save > 3000) {
            svc::library().save_if_dirty();
            last_save = now_ms();
        }
    }
    svc::library().save_if_dirty();
}

}  // namespace ui
