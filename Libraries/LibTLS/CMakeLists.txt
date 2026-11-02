#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# A TLS 1.3 client and the X.509 certificates it checks. Reads and writes through a Transport
# of the caller's, so that none of it depends on a socket.
add_library(LibTLS STATIC
    ASN1.cpp
    Certificate.cpp
    Connection.cpp)
target_link_libraries(LibTLS PUBLIC LibCrypto LibBase)
