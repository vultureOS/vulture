#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Hashes, message authentication, key derivation, ciphers and public keys: for passwords,
# for TLS, and for checking what is signed.
if(VULTURE_PART STREQUAL "target")
    # The kernel only checks signatures, of its own system's seal.
    add_library(LibCrypto STATIC
        Curve25519.cpp
        SHA2.cpp)
else()
    add_library(LibCrypto STATIC
        AES.cpp
        BigInteger.cpp
        ChaCha20Poly1305.cpp
        Curve25519.cpp
        EllipticCurve.cpp
        GCM.cpp
        RSA.cpp
        SHA2.cpp)
endif()
