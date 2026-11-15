// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/troubleshoot.h"

#include "core/text.h"
#include "gfx/font.h"
#include "net/url.h"
#include "services/settings.h"
#include "ui/app.h"
#include "ui/widgets.h"

namespace ui {

using gfx::Face;
using gfx::Rect;
using namespace platform;

namespace {

std::string host_of(const std::string& url) {
    lib::Url u;
    return lib::parse_url(lib::normalise_server_url(url), u) ? u.host : std::string();
}

// When a server does not answer: if the other servers in Settings are all on
// one machine and this address names another, it may be a typing mistake
// (192.168.1.171 for 192.168.1.141). Returns a step saying so, or "".
std::string other_servers_hint(Service service) {
    const svc::Settings& s = svc::settings();
    const std::string mine = host_of(service == Service::Kiwix ? s.kiwix_url
                                     : service == Service::Calibre ? s.calibre_url
                                                                   : s.abs_url);
    std::string other;
    for (const std::string* url : {&s.kiwix_url, &s.calibre_url, &s.abs_url}) {
        const std::string h = host_of(*url);
        if (h.empty() || h == mine) continue;
        if (!other.empty() && other != h) return "";  // several machines: no guess
        other = h;
    }
    if (other.empty() || mine.empty()) return "";
    return "Your other servers are at " + other + ", but this one is set to " + mine +
           ". If it runs on the same machine, the address should start with http://" + other + ".";
}

Diagnosis diagnose_audiobookshelf(int status, const std::string& error) {
    const svc::Settings& s = svc::settings();
    const std::string address = lib::normalise_server_url(s.abs_url);
    Diagnosis d;
    if (status >= 200 && status < 300 && error.empty()) return d;
    if (status == 0) {
        // Nothing was sent: something is missing in Settings.
        d.problem = Problem::SignInRequired;
        d.title = error.empty() ? "Audiobookshelf needs an account" : error;
        d.steps = {
            "Enter the address of your Audiobookshelf server, for example http://192.168.1.10:13378.",
            "Enter the user name and password you use in Audiobookshelf's web page, or an API key made there "
            "(Settings › API Keys, Audiobookshelf 2.26 or newer).",
        };
        return d;
    }
    if (status >= 200 && status < 300) {
        d.problem = Problem::WrongService;
        d.title = "Something answered at " + address + ", but it is not Audiobookshelf";
        d.steps = {
            "Check the port: Audiobookshelf usually uses 13378 (80 inside Docker, published as another port). Another "
            "app may be answering instead.",
            "If Audiobookshelf is behind a proxy under a path (such as /audiobookshelf), include that path in the address.",
            "Open the address in a browser on your phone or computer: it should show Audiobookshelf's sign-in page.",
        };
        return d;
    }
    if (status < 0) {
        const bool tls = lib::starts_with(address, "https") || lib::icontains(error, "certificate");
        d.problem = tls ? Problem::Certificate : Problem::NoAnswer;
        d.title = "No answer from " + (address.empty() ? std::string("Audiobookshelf") : address);
        d.steps = {
            "Check the address and port for typing mistakes. Audiobookshelf usually uses port 13378.",
            "Use the server's IP address (for example 192.168.1.10). Names ending in .local cannot be looked up by the PS4.",
            "Make sure the server is running: open the address in a browser on your phone or computer.",
            "The PS4 and the server must be on the same network.",
        };
        if (tls)
            d.steps.push_back("The address uses HTTPS. Try http:// instead, or, for a server with a self-signed certificate, "
                              "allow self-signed certificates under Network.");
        const std::string hint = other_servers_hint(Service::Audiobookshelf);
        if (!hint.empty()) d.steps.insert(d.steps.begin(), hint);
        return d;
    }
    if (status == 401) {
        d.problem = Problem::SignInRejected;
        d.title = s.abs_api_key.empty() ? "Audiobookshelf did not accept the user name or password"
                                        : "Audiobookshelf did not accept the API key";
        d.steps = {
            s.abs_api_key.empty() ? "Check the user name and password: the password is case-sensitive."
                                  : "Check the API key in Audiobookshelf's Settings › API Keys: it may have expired or been "
                                    "deactivated. Leave the key empty to sign in with a user name and password instead.",
            "Sign in with the same account in a browser to make sure it works and is not locked.",
            "Audiobookshelf's own log (its web page, Settings \xE2\x80\xBA Logs) has a line for each failed sign-in from the "
            "PS4 saying whether the user was not found or the password was wrong.",
        };
        return d;
    }
    if (status == 400) {
        d.problem = Problem::Other;
        d.title = "Audiobookshelf did not receive the sign-in details";
        d.steps = {"Re-enter the password in Settings and try again.",
                   "If it keeps happening, please send Librarian's log (/data/Librarian/user-<id>/librarian.log): "
                   "the request may not be reaching the server intact."};
        return d;
    }
    if (status == 429) {
        d.problem = Problem::RateLimited;
        d.title = "Too many sign-in attempts";
        d.steps = {"Audiobookshelf limits sign-in attempts. Wait ten minutes, check the password, and try once more."};
        return d;
    }
    if (status == 403) {
        d.problem = Problem::NoPermission;
        d.title = "Audiobookshelf refused access";
        d.steps = {"The account lacks a permission (for example to download, or to see this library). An administrator "
                   "can change it in Audiobookshelf's Settings › Users."};
        return d;
    }
    if (status == 404) {
        d.problem = Problem::NotFound;
        d.title = "Audiobookshelf has nothing at this address";
        d.steps = {"Use the address of Audiobookshelf itself (http://IP:13378), without a page at the end.",
                   "Librarian needs Audiobookshelf 2.0 or newer."};
        return d;
    }
    if (status >= 500) {
        d.problem = Problem::ServerError;
        d.title = "Audiobookshelf had an internal error (" + std::to_string(status) + ")";
        d.steps = {"Try again in a moment.", "If it keeps happening, restart Audiobookshelf and check its logs (Settings › Logs)."};
        return d;
    }
    d.problem = Problem::Other;
    d.title = "Audiobookshelf answered with status " + std::to_string(status);
    d.steps = {"Check the address in Settings, and open it in a browser to see what the server says."};
    return d;
}

}  // namespace

Diagnosis diagnose(Service service, int status, const std::string& error) {
    if (service == Service::Audiobookshelf) return diagnose_audiobookshelf(status, error);
    const svc::Settings& s = svc::settings();
    const bool kiwix = service == Service::Kiwix;
    const std::string name = kiwix ? "Kiwix Server" : "Calibre-Web";
    const std::string address = lib::normalise_server_url(kiwix ? s.kiwix_url : s.calibre_url);
    const std::string port = kiwix ? "8080" : "8083";
    Diagnosis d;

    if (status >= 200 && status < 300 && error.empty()) return d;
    if (status >= 200 && status < 300) {
        d.problem = Problem::WrongService;
        d.title = "Something answered at " + address + ", but it is not " + name;
        d.steps = {
            "Check the port: " + name + " usually uses port " + port + ". Another app (such as the CasaOS dashboard "
            "on port 80) may be answering instead.",
            kiwix ? "Use the server's own address (http://IP:" + port + "), not a link to one collection. Librarian needs "
                    "kiwix-serve 3.2 or newer."
                  : "Use the address of Calibre-Web itself (http://IP:" + port + "), without /opds or a page at the end, and "
                    "make sure OPDS is not turned off in Calibre-Web's configuration.",
            "Open the address in a browser on your phone or computer: it should show " + name + ".",
        };
        return d;
    }
    if (status < 0) {
        const bool tls = lib::starts_with(address, "https") || lib::icontains(error, "certificate");
        d.problem = tls ? Problem::Certificate : Problem::NoAnswer;
        d.title = "No answer from " + (address.empty() ? name : address);
        d.steps = {
            "Check the address and port for typing mistakes. " + name + " usually uses port " + port +
                "; in CasaOS the port is shown when you open the app.",
            "Use the server's IP address (for example 192.168.1.10). Names ending in .local cannot be looked up by the PS4.",
            "Make sure the server is running: open the address in a browser on your phone or computer.",
            "The PS4 and the server must be on the same network. The PS4's address is in its Settings › Network › "
            "View Connection Status.",
        };
        if (tls)
            d.steps.push_back("The address uses HTTPS. Try http:// instead, or, for a server with a self-signed certificate, "
                              "allow self-signed certificates under Network.");
        const std::string hint = other_servers_hint(service);
        if (!hint.empty()) d.steps.insert(d.steps.begin(), hint);
        return d;
    }
    if (status == 401) {
        if (kiwix) {
            d.problem = Problem::SignInRequired;
            d.title = "The server asked for a password";
            d.steps = {"Kiwix Server itself has no sign-in: a proxy in front of it is asking for one. Use Kiwix Server's own "
                       "address and port (usually " + port + ") instead."};
            return d;
        }
        if (s.calibre_anonymous || s.calibre_user.empty()) {
            d.problem = Problem::SignInRequired;
            d.title = "Calibre-Web asked for a sign-in";
            d.steps = {
                "To browse without an account, turn on anonymous browsing in Calibre-Web: Admin › Edit Basic Configuration › "
                "Feature Configuration › Enable Anonymous Browsing, then Save.",
                "Let guests download books: Admin › Users › Guest › tick Allow Downloads.",
                "Or set Sign-in to \"With an account\" here and enter your Calibre-Web user name and password.",
            };
        } else {
            d.problem = Problem::SignInRejected;
            d.title = "Calibre-Web did not accept the user name or password";
            d.steps = {
                "Check the user name and password: both are case-sensitive.",
                "Sign in with the same account in a browser to make sure it works.",
                "If your server allows anonymous browsing, you can set Sign-in to \"Anonymous\" instead.",
            };
        }
        return d;
    }
    if (status == 403) {
        d.problem = Problem::NoPermission;
        d.title = name + " refused access";
        d.steps = kiwix ? std::vector<std::string>{"The server or a proxy in front of it refuses this device. Check its "
                                                   "access settings."}
                        : std::vector<std::string>{
                              "The account (or the Guest user, when browsing anonymously) lacks permission. In Calibre-Web, "
                              "Admin › Users: allow Downloads, and make sure the needed sections are visible.",
                          };
        return d;
    }
    if (status == 404) {
        d.problem = Problem::NotFound;
        d.title = "The server has no " + std::string(kiwix ? "Kiwix catalogue" : "OPDS catalogue") + " at this address";
        d.steps = {
            kiwix ? "Use the server's own address (http://IP:" + port + "), not a link to a collection. Librarian needs "
                    "kiwix-serve 3.2 or newer (older versions have no OPDS v2 catalogue)."
                  : "Use the address of Calibre-Web itself (http://IP:" + port + "), without /opds at the end.",
            "Check the port: another app on the same server may be answering.",
        };
        return d;
    }
    if (status >= 500) {
        d.problem = Problem::ServerError;
        d.title = name + " had an internal error (" + std::to_string(status) + ")";
        d.steps = {"Try again in a moment.", "If it keeps happening, restart " + name + " (in CasaOS: open the app's menu and "
                                                                                     "choose Restart) and check its logs."};
        return d;
    }
    d.problem = Problem::Other;
    d.title = name + " answered with status " + std::to_string(status);
    d.steps = {"Check the address in Settings, and open it in a browser to see what the server says."};
    return d;
}

namespace {

class TroubleshootScreen : public Screen {
public:
    TroubleshootScreen(Service service, int status, std::string error)
        : service_(service), error_(std::move(error)), d_(diagnose(service, status, error_)) {}

    bool overlay() const override { return true; }
    std::vector<Hint> hints() const override { return {{kCircle, "Close"}, {kCross, "Open Settings"}}; }

    void update(const Input& in) override {
        if (in.press(kCircle)) app().pop();
        else if (in.press(kCross)) app().switch_tab(Tab::Settings);
    }

    void draw(gfx::Canvas& c) override {
        const Theme& t = theme();
        c.fill({0, 0, c.width(), c.height()}, gfx::with_alpha(gfx::rgb(0x05070B), 180));
        const int w = 1240, x = (c.width() - w) / 2, pad = 56;
        // Measure first, then draw the panel to fit.
        int h = 120;
        const auto title = gfx::wrap_text(Face::SansBold, 38, d_.title, w - 2 * pad, 2);
        h += (int)title.size() * 50 + 70;
        std::vector<std::vector<std::string>> steps;
        for (const auto& s : d_.steps) {
            steps.push_back(gfx::wrap_text(Face::Sans, 26, s, w - 2 * pad - 60, 5));
            h += (int)steps.back().size() * 38 + 24;
        }
        const int y0 = std::max(60, (c.height() - h) / 2);
        c.fill_rounded({x, y0, w, h}, 22, t.surface);
        int y = y0 + 44;
        gfx::draw_text(c, Face::SansSemiBold, 24, x + pad, y,
                       service_ == Service::Kiwix     ? "TROUBLESHOOTING  \xC2\xB7  KIWIX"
                       : service_ == Service::Calibre ? "TROUBLESHOOTING  \xC2\xB7  CALIBRE-WEB"
                                                      : "TROUBLESHOOTING  \xC2\xB7  AUDIOBOOKSHELF",
                       t.accent);
        y += 52;
        for (const auto& line : title) {
            gfx::draw_text(c, Face::SansBold, 38, x + pad, y, line, t.text);
            y += 50;
        }
        y += 6;
        gfx::draw_text(c, Face::Sans, 22, x + pad, y, gfx::ellipsize(Face::Sans, 22, error_, w - 2 * pad), t.faint);
        y += 64;
        for (size_t i = 0; i < steps.size(); ++i) {
            c.fill_circle(x + pad + 18, y + 18, 18, t.surface_hi);
            const std::string n = std::to_string(i + 1);
            gfx::draw_text(c, Face::SansBold, 22, x + pad + 18 - gfx::text_width(Face::SansBold, 22, n) / 2, y + 3, n, t.text);
            for (const auto& line : steps[i]) {
                gfx::draw_text(c, Face::Sans, 26, x + pad + 60, y, line, t.text);
                y += 38;
            }
            y += 24;
        }
    }

private:
    Service service_;
    std::string error_;
    Diagnosis d_;
};

}  // namespace

void open_troubleshooter(Service service, int status, const std::string& error) {
    app().push(std::make_unique<TroubleshootScreen>(service, status, error));
}

}  // namespace ui
