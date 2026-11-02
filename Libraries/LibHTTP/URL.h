/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>

namespace HTTP {

struct URL {
    String scheme; // "http" or "https", in lower case.
    String host;   // In lower case.
    u16 port { 0 }; // As given, or the scheme's.
    String target; // What the request line asks for: the path and query, at least "/".

    // scheme://host[:port][/path][?query], with http assumed if there is no scheme. A fragment
    // is dropped, as it is never sent; a user name in it is refused, as a way of misleading.
    [[nodiscard]] static ErrorOr<URL> parse(StringView);
    // Where a redirect to `location` goes from here: absolute, from the root, or relative.
    [[nodiscard]] ErrorOr<URL> resolve(StringView location) const;

    [[nodiscard]] bool is_https() const { return scheme.view() == "https"_sv; }
    // As for a Host header: no port if it is the scheme's usual one.
    [[nodiscard]] String host_header() const;
    [[nodiscard]] String to_string() const;
    // What the end of the path calls itself, for "curl -O".
    [[nodiscard]] String file_name() const;
};

}
