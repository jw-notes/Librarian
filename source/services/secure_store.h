// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#pragma once
// Encrypted files, for settings.json (which holds passwords and keys).
//
// XChaCha20-Poly1305 (Monocypher). The key is BLAKE2b of a constant, this
// console's ID (platform::device_secret) and a random value kept beside the
// file (<data>/.key). A copy of settings.json taken off the console (over
// FTP, in a backup, attached to a bug report) cannot be read or altered
// without both. It is not a defence against code running on the console
// itself, which can derive the same key.
//
// Layout: "LIBRENC1", 24-byte nonce, 16-byte MAC, ciphertext.
#include <string>

namespace svc {

enum class SecureRead {
    Missing,    // no such file
    Plain,      // a plain (older, unencrypted) file: returned as it is
    Decrypted,  // decrypted and authentic
    Failed      // encrypted, but not with this console's key, or altered
};

SecureRead read_secure(const std::string& path, std::string& out);
bool write_secure(const std::string& path, const std::string& plain);
// True when `data` starts like an encrypted file.
bool is_secure(const std::string& data);

}  // namespace svc
