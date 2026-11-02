/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/StringView.h>
#include <LibCrypto/BigInteger.h>

// Checking RSA signatures (RFC 8017): what certificates are signed with. Only the public
// half: nothing here signs.

namespace Crypto {

enum class SignatureHash : u8 {
    SHA256,
    SHA384,
    SHA512,
};

struct RSAPublicKey {
    BigInteger modulus;
    BigInteger exponent;
};

// RSASSA-PKCS1-v1_5.
[[nodiscard]] bool rsa_verify_pkcs1(RSAPublicKey const&, SignatureHash, ReadonlyBytes message, ReadonlyBytes signature);
// RSASSA-PSS, with MGF1 over the same hash and a salt as long as the digest.
[[nodiscard]] bool rsa_verify_pss(RSAPublicKey const&, SignatureHash, ReadonlyBytes message, ReadonlyBytes signature);

}
