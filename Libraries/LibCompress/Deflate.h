/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>

// DEFLATE (RFC 1951), and its zlib (RFC 1950, for PNG) and gzip (RFC 1952) wrappings.

namespace Compress {

// More than this is refused, whatever the input claims.
inline constexpr size_t default_output_limit = 1024ull * 1024 * 1024;

[[nodiscard]] ErrorOr<Vector<u8>> inflate(ReadonlyBytes, size_t output_limit = default_output_limit);
[[nodiscard]] ErrorOr<Vector<u8>> zlib_decompress(ReadonlyBytes, size_t output_limit = default_output_limit);

[[nodiscard]] ErrorOr<Vector<u8>> deflate(ReadonlyBytes);
[[nodiscard]] ErrorOr<Vector<u8>> zlib_compress(ReadonlyBytes);

// One member: its checksum and length are checked.
[[nodiscard]] ErrorOr<Vector<u8>> gzip_decompress(ReadonlyBytes, size_t output_limit = default_output_limit);
[[nodiscard]] ErrorOr<Vector<u8>> gzip_compress(ReadonlyBytes);

[[nodiscard]] u32 adler32(ReadonlyBytes);

}
