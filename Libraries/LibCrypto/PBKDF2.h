/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibCrypto/HMAC.h>

namespace Crypto {

// PBKDF2 (RFC 8018) with HMAC over `Hash`: fills `output` with the key
// derived from `password` and `salt` in `iterations` rounds.
template<typename Hash>
void pbkdf2(ReadonlyBytes password, ReadonlyBytes salt, u32 iterations, Bytes output)
{
    HMAC<Hash> hmac(password);
    size_t done = 0;
    for (u32 block = 1; done < output.size(); ++block) {
        u8 index[4] = { static_cast<u8>(block >> 24), static_cast<u8>(block >> 16), static_cast<u8>(block >> 8), static_cast<u8>(block) };
        hmac.update(salt);
        hmac.update({ index, 4 });
        auto u = hmac.digest();
        auto t = u;
        for (u32 round = 1; round < iterations; ++round) {
            hmac.update(u.span());
            u = hmac.digest();
            for (size_t i = 0; i < Hash::digest_size; ++i)
                t[i] ^= u[i];
        }
        size_t take = output.size() - done < Hash::digest_size ? output.size() - done : Hash::digest_size;
        __builtin_memcpy(output.data() + done, t.data(), take);
        done += take;
    }
}

}
