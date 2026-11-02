#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# HTTP/1.1. What only looks at bytes (URLs, reading a response) also builds for the host, for
# its tests; Client is what talks to the network.
set(LibHTTP_SOURCES
    Response.cpp
    URL.cpp)
if(VULTURE_PART STREQUAL "userland")
    list(APPEND LibHTTP_SOURCES Client.cpp)
endif()
add_library(LibHTTP STATIC ${LibHTTP_SOURCES})
target_link_libraries(LibHTTP PUBLIC LibBase)
if(VULTURE_PART STREQUAL "userland")
    target_link_libraries(LibHTTP PUBLIC LibTLS LibCrypto LibCompress)
endif()
