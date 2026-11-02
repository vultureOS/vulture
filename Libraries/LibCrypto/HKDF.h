/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibCrypto/HMAC.h>

namespace Crypto {

// HKDF (RFC 5869) over any of the hashes in SHA2.h.
template<typename Hash>
[[nodiscard]] typename Hash::Digest hkdf_extract(ReadonlyBytes salt, ReadonlyBytes input_key)
{
    u8 zeroes[Hash::digest_size] = {};
    HMAC<Hash> hmac(salt.is_empty() ? ReadonlyBytes { zeroes, Hash::digest_size } : salt);
    hmac.update(input_key);
    return hmac.digest();
}

template<typename Hash>
void hkdf_expand(ReadonlyBytes pseudorandom_key, ReadonlyBytes info, Bytes output)
{
    HMAC<Hash> hmac(pseudorandom_key);
    typename Hash::Digest previous {};
    size_t done = 0;
    for (u8 counter = 1; done < output.size(); ++counter) {
        if (counter > 1)
            hmac.update(previous.span());
        hmac.update(info);
        hmac.update({ &counter, 1 });
        previous = hmac.digest();
        size_t take = output.size() - done < Hash::digest_size ? output.size() - done : Hash::digest_size;
        __builtin_memcpy(output.data() + done, previous.data(), take);
        done += take;
    }
}

}
