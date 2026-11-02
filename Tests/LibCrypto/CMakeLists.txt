#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibCrypto
    TestCrypto.cpp
    TestPublicKey.cpp)

target_link_libraries(TestLibCrypto PRIVATE LibTest LibCrypto LibBase)
add_test(NAME LibCrypto COMMAND TestLibCrypto)
