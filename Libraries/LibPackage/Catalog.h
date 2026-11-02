/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibJSON/JSON.h>

// What a repository offers. A repository is a folder of packages at a URL, and in it
// "index.json", which says what they are, how long, and their SHA-256; and "index.json.sig", an
// Ed25519 signature of those bytes by the repository's key. Checking that one signature
// therefore vouches for every package named in it.

namespace Package {

struct Available {
    String name;
    String version;
    String summary;
    // Where it comes from, whole.
    String url;
    String sha256; // In hexadecimal.
    u64 size { 0 };
    String repository;
};

// An index, once its signature has been checked. `base_url` is what its relative URLs are
// relative to. Each name may be only letters, digits, "-", "_" and ".".
[[nodiscard]] ErrorOr<Vector<Available>> parse_index(ReadonlyBytes json, StringView base_url, StringView repository_name, u64* serial = nullptr);
// Whether `signature` is `key`'s, of those bytes. The key and the signature are hexadecimal text.
[[nodiscard]] bool verify_signature(ReadonlyBytes data, StringView signature_hex, StringView public_key_hex);
[[nodiscard]] bool is_valid_name(StringView);

// Whether a package may put a file here, relative to the root: only under /Applications and
// /usr/local, and only by plain, relative paths.
[[nodiscard]] bool is_allowed_install_path(StringView);

[[nodiscard]] String hex_of(ReadonlyBytes);

}
