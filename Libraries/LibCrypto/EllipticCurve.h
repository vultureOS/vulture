/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Types.h>

// The NIST curves P-256 and P-384 (FIPS 186-4), for key agreement and for checking ECDSA
// signatures. One piece of code does both curves, through complete addition formulas
// (Renes, Costello and Batina, 2015) and a Montgomery ladder: no exceptions to handle, and
// the same steps whatever a secret scalar is.

namespace Crypto {

enum class NamedCurve : u8 {
    P256,
    P384,
};

// 32 or 48: the size of one coordinate. A public key is 0x04, x, y.
[[nodiscard]] size_t curve_coordinate_size(NamedCurve);

// `scalar` is a coordinate's size, big-endian. False if it is zero or not below the order.
[[nodiscard]] bool ec_public_key(NamedCurve, ReadonlyBytes scalar, Bytes public_key);
// The x-coordinate of `scalar` times the peer's point. False if that isn't on the curve, or
// the answer is the point at infinity.
[[nodiscard]] bool ecdh(NamedCurve, ReadonlyBytes scalar, ReadonlyBytes peer_public_key, Bytes shared_secret);
// Signature (r, s), each big-endian without leading zeroes needing to be there. `digest` is
// cut to the curve's size if longer.
[[nodiscard]] bool ecdsa_verify(NamedCurve, ReadonlyBytes public_key, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s);

}
