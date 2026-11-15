// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// SHA-256 (FIPS 180-4), for checking downloaded update packages.
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace lib {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    // The digest as 64 lower-case hex digits.
    std::string hex();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t bytes_ = 0;
};

// The SHA-256 of a whole file (lib::open_file, so USB through FTP too); "" when unreadable.
std::string sha256_file(const std::string& path);

}  // namespace lib
