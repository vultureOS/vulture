/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Array.h>
#include <LibBase/Span.h>
#include <LibBase/Types.h>

namespace Crypto {

// SHA-256 and SHA-512 (FIPS 180-4). Feed bytes with update(), then take
// the digest once; reset() starts over.
class SHA256 {
public:
    static constexpr size_t digest_size = 32;
    static constexpr size_t block_size = 64;
    using Digest = Array<u8, digest_size>;

    SHA256() { reset(); }
    void reset();
    void update(ReadonlyBytes);
    [[nodiscard]] Digest digest();

    [[nodiscard]] static Digest hash(ReadonlyBytes data)
    {
        SHA256 hasher;
        hasher.update(data);
        return hasher.digest();
    }

private:
    void transform(u8 const* block);

    u32 m_state[8];
    u8 m_buffer[block_size];
    size_t m_buffered { 0 };
    u64 m_length { 0 };
};

class SHA512 {
public:
    static constexpr size_t digest_size = 64;
    static constexpr size_t block_size = 128;
    using Digest = Array<u8, digest_size>;

    SHA512() { reset(); }
    void reset();
    void update(ReadonlyBytes);
    [[nodiscard]] Digest digest();

    [[nodiscard]] static Digest hash(ReadonlyBytes data)
    {
        SHA512 hasher;
        hasher.update(data);
        return hasher.digest();
    }

private:
    void transform(u8 const* block);

    u64 m_state[8];
    u8 m_buffer[block_size];
    size_t m_buffered { 0 };
    u64 m_length { 0 }; // Bytes; messages over 2^61 bytes are not expected.
};

}
