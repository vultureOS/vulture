/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Vector.h>

namespace Crypto {

// A big natural number, for checking RSA signatures. What goes through this is public, so
// nothing here tries to take the same time whatever the values are: it is not for secrets.
class BigInteger {
public:
    BigInteger() = default;

    // Big-endian bytes, as DER and RSA have them.
    [[nodiscard]] static BigInteger from_bytes(ReadonlyBytes);
    // Fills `output`, with zeroes in front; false if the number does not fit.
    [[nodiscard]] bool to_bytes(Bytes output) const;

    [[nodiscard]] bool is_zero() const { return m_words.is_empty(); }
    [[nodiscard]] bool is_odd() const { return !m_words.is_empty() && (m_words[0] & 1); }
    [[nodiscard]] size_t bit_length() const;
    [[nodiscard]] int compare(BigInteger const&) const;

    // base^exponent modulo this odd number, by Montgomery multiplication.
    [[nodiscard]] BigInteger modular_power_of(BigInteger const& base, BigInteger const& exponent) const;

private:
    void trim();
    [[nodiscard]] BigInteger reduced(BigInteger const& value) const;

    Vector<u32> m_words; // Little end first; no zero at the top.
};

}
