#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Hashes, message authentication and key derivation, for passwords now
# and TLS later.
add_library(LibCrypto STATIC
    SHA2.cpp)
