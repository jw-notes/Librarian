// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Software updates (Settings › System › Software update), done the way the
// author's other PS4 homebrew does them: the project's latest GitHub release
// is compared with this build; a newer one's .pkg is downloaded to /data/pkg
// (GoldHEN's Package Installer lists that folder), its size and the SHA-256
// GitHub publishes for it are checked, and the user installs it. Librarian
// never installs anything itself.
#include <stdint.h>

#include <string>

namespace svc::updater {

// The GitHub repository releases come from (not public yet: until then the
// check finds no release, which is not an error).
constexpr const char* kRepo = "jw-notes/Librarian";
constexpr const char* kFolder = "/data/pkg";
constexpr const char* kFile = "/data/pkg/Librarian.pkg";

enum class State { Idle, Checking, Current, Available, Downloading, Downloaded, Failed };

// Checks in the background; `delay_ms` first (at startup, so it never
// competes with loading). Call from the UI thread.
void check(int delay_ms = 0);
void download();
void cancel();
State state();
// The newer release's version ("00.11"), once Available.
std::string available_version();
// Why the check or the download failed (English; translated where shown).
std::string error();
void progress(uint64_t& done, uint64_t& total);
// This build's version.
std::string current_version();
// Stops the background work (at exit).
void shutdown();

// "v00.11" vs "00.10": true when `release` is newer (exposed for tests).
bool newer(const std::string& release, const std::string& running);
// Reads GitHub's "latest release" answer: tag, the .pkg's address, size and
// SHA-256 (exposed for tests).
bool parse_release(const std::string& json, std::string& tag, std::string& url, uint64_t& size, std::string& sha256);

}  // namespace svc::updater
