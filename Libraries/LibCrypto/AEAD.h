/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Types.h>

// Authenticated encryption, as TLS 1.3 uses it (RFC 5116's interface): AES-GCM and
// ChaCha20-Poly1305. A 12-byte nonce, never twice with one key, and a 16-byte tag.

namespace Crypto {

inline constexpr size_t aead_nonce_size = 12;
inline constexpr size_t aead_tag_size = 16;

class AEAD {
public:
    virtual ~AEAD() = default;

    // `output` holds as many bytes as `plaintext`; it may be the same place. `tag` holds
    // aead_tag_size.
    virtual void seal(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes plaintext, u8* output, u8* tag) const = 0;
    // False if the tag is wrong, and then `output` is zeroes.
    [[nodiscard]] virtual bool open(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8 const* tag, u8* output) const = 0;
};

}
