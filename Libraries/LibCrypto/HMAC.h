/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>

namespace Crypto {

// HMAC (RFC 2104) over any of the hashes in SHA2.h.
template<typename Hash>
class HMAC {
public:
    using Digest = typename Hash::Digest;

    explicit HMAC(ReadonlyBytes key)
    {
        u8 block[Hash::block_size] = {};
        if (key.size() > Hash::block_size) {
            auto hashed = Hash::hash(key);
            __builtin_memcpy(block, hashed.data(), Hash::digest_size);
        } else {
            __builtin_memcpy(block, key.data(), key.size());
        }
        for (size_t i = 0; i < Hash::block_size; ++i) {
            m_inner_pad[i] = block[i] ^ 0x36;
            m_outer_pad[i] = block[i] ^ 0x5c;
        }
        __builtin_memset(block, 0, sizeof(block));
        reset();
    }

    ~HMAC()
    {
        __builtin_memset(m_inner_pad, 0, sizeof(m_inner_pad));
        __builtin_memset(m_outer_pad, 0, sizeof(m_outer_pad));
    }

    void reset()
    {
        m_inner.reset();
        m_inner.update({ m_inner_pad, Hash::block_size });
    }

    void update(ReadonlyBytes data) { m_inner.update(data); }

    [[nodiscard]] Digest digest()
    {
        auto inner = m_inner.digest();
        Hash outer;
        outer.update({ m_outer_pad, Hash::block_size });
        outer.update(inner.span());
        reset();
        return outer.digest();
    }

private:
    Hash m_inner;
    u8 m_inner_pad[Hash::block_size];
    u8 m_outer_pad[Hash::block_size];
};

// Compares in time that depends only on the length, for secrets.
inline bool constant_time_equal(ReadonlyBytes a, ReadonlyBytes b)
{
    if (a.size() != b.size())
        return false;
    u8 difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference |= a[i] ^ b[i];
    return difference == 0;
}

}
