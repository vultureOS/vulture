/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibCrypto/AEAD.h>
#include <LibCrypto/AES.h>

namespace Crypto {

// AES-GCM (NIST SP 800-38D), with a key of 16 or 32 bytes.
class AESGCM final : public AEAD {
public:
    explicit AESGCM(ReadonlyBytes key);
    ~AESGCM() override;

    void seal(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes plaintext, u8* output, u8* tag) const override;
    [[nodiscard]] bool open(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8 const* tag, u8* output) const override;

private:
    void compute_tag(u8 const* nonce, ReadonlyBytes associated_data, ReadonlyBytes ciphertext, u8* tag) const;
    void apply_counter_mode(u8 const* nonce, ReadonlyBytes input, u8* output) const;

    AES m_cipher;
    u64 m_h_high { 0 };
    u64 m_h_low { 0 };
};

}
