#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Hashes, message authentication, key derivation, ciphers and public keys: for passwords,
# for TLS, and for checking what is signed.
add_library(LibCrypto STATIC
    AES.cpp
    BigInteger.cpp
    ChaCha20Poly1305.cpp
    Curve25519.cpp
    EllipticCurve.cpp
    GCM.cpp
    RSA.cpp
    SHA2.cpp)
