/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibCrypto/AEAD.h>

namespace Crypto {

// ChaCha20-Poly1305 (RFC 8439), with a key of 32 bytes.
class ChaCha20Poly1305 final : public AEAD {
public:
    explicit ChaCha20Poly1305(ReadonlyBytes key);
    ~ChaCha20Poly1305() override;

    void seal(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes plaintext, u8* output, u8* tag) const override;
    [[nodiscard]] bool open(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8 const* tag, u8* output) const override;

private:
    void compute_tag(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8* tag) const;

    u8 m_key[32];
};

// The block function alone, for tests: 64 bytes of keystream.
void chacha20_block(u8 const* key, u32 counter, u8 const* nonce, u8* output);
// One-time authenticator, for tests: `tag` holds 16 bytes.
void poly1305(u8 const* key, ReadonlyBytes message, u8* tag);

}
