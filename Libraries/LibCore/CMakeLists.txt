#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What programs share above the C library: user accounts now, the event
# loop later.
add_library(LibCore STATIC
    Account.cpp
    EventLoop.cpp
    GetPassword.cpp)
target_link_libraries(LibCore PUBLIC LibCrypto)
