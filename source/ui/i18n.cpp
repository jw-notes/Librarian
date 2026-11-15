// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "ui/i18n.h"

#include <string.h>

#include <string>
#include <unordered_map>

#include "core/log.h"
#include "platform/platform.h"
#include "services/settings.h"

namespace ui {

// i18n_catalog.cpp: {English, 日本語, Português (Brasil), Deutsch, Français, Español}.
struct CatalogEntry {
    const char* text[(int)Lang::Count];
};
extern const CatalogEntry kCatalog[];
extern const size_t kCatalogSize;

namespace {

Lang g_lang = Lang::En;

const char* kCodes[] = {"en", "ja", "pt-BR", "de", "fr", "es"};
const char* kNames[] = {"English", "日本語", "Português (Brasil)", "Deutsch", "Français", "Español"};

const std::unordered_map<std::string, const CatalogEntry*>& index() {
    static std::unordered_map<std::string, const CatalogEntry*> map = [] {
        std::unordered_map<std::string, const CatalogEntry*> m;
        for (size_t i = 0; i < kCatalogSize; ++i) m[kCatalog[i].text[0]] = &kCatalog[i];
        return m;
    }();
    return map;
}

int code_index(const std::string& code) {
    for (int i = 0; i < (int)Lang::Count; ++i)
        if (code == kCodes[i]) return i;
    return -1;
}

Lang system_language() {
    const int i = code_index(platform::system_language());
    return i < 0 ? Lang::En : (Lang)i;
}

// The printf conversions of a format, in order ("%s%d").
std::string conversions(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.lhzjt", *p)) ++p;
        if (*p) out += *p;
        else break;
    }
    return out;
}

}  // namespace

const char* T(const char* en) {
    if (!en || !*en || g_lang == Lang::En) return en;
    const auto& m = index();
    auto it = m.find(en);
    if (it == m.end()) return en;
    const char* t = it->second->text[(int)g_lang];
    return t && *t ? t : en;
}

std::string T(const std::string& en) {
    if (en.empty() || g_lang == Lang::En) return en;
    const auto& m = index();
    auto it = m.find(en);
    if (it == m.end()) return en;
    const char* t = it->second->text[(int)g_lang];
    return t && *t ? std::string(t) : en;
}

Lang current_language() { return g_lang; }

void language_apply() {
    const int i = code_index(svc::settings().language);
    g_lang = i >= 0 ? (Lang)i : system_language();
    LOG("language: %s%s", kCodes[(int)g_lang], i >= 0 ? "" : " (the console's)");
}

// Auto, then each language.
void language_step(int dir) {
    int pref = code_index(svc::settings().language);  // -1: auto
    const int n = (int)Lang::Count + 1;
    pref = ((pref + 1) + dir + n) % n - 1;
    svc::settings().language = pref < 0 ? "auto" : kCodes[pref];
    language_apply();
}

std::string language_label() {
    const int i = code_index(svc::settings().language);
    if (i >= 0) return kNames[i];
    return std::string(T("Auto")) + " (" + kNames[(int)system_language()] + ")";
}

CatalogCheck check_catalog() {
    CatalogCheck c;
    c.entries = kCatalogSize;
    std::unordered_map<std::string, int> seen;
    for (size_t i = 0; i < kCatalogSize; ++i) {
        const CatalogEntry& e = kCatalog[i];
        if (++seen[e.text[0]] > 1) {
            c.problem = std::string("twice: ") + e.text[0];
            return c;
        }
        const std::string want = conversions(e.text[0]);
        for (int l = 1; l < (int)Lang::Count; ++l) {
            if (!e.text[l] || !*e.text[l]) {
                c.problem = std::string("no ") + kCodes[l] + ": " + e.text[0];
                return c;
            }
            if (conversions(e.text[l]) != want) {
                c.problem = std::string("conversions differ in ") + kCodes[l] + ": " + e.text[0];
                return c;
            }
        }
    }
    return c;
}

}  // namespace ui
