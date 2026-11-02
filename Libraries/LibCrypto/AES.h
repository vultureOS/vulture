/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Types.h>

namespace Crypto {

// AES (FIPS 197), encrypting only: GCM needs nothing else. Uses the processor's own
// instructions where it has them. Without them, every S-box lookup reads the whole table,
// so what is secret doesn't show in which cache lines were touched, at a price in speed.
class AES {
public:
    static constexpr size_t block_size = 16;

    // A key of 16 or 32 bytes.
    explicit AES(ReadonlyBytes key);
    ~AES();

    void encrypt_block(u8 const* input, u8* output) const;

    // Whether this processor has the instructions. For choosing ChaCha20 when it hasn't,
    // which is then both faster and as safe.
    [[nodiscard]] static bool has_hardware();
    // For tests: run the plain code even where there is a choice.
    static void set_hardware_allowed(bool);

private:
    static constexpr size_t largest_round_count = 14;

    u8 m_round_keys[(largest_round_count + 1) * block_size];
    int m_rounds { 0 };
};

}
