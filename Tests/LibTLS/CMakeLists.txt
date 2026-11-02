#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibTLS TestCertificate.cpp)
target_link_libraries(TestLibTLS PRIVATE LibTest LibTLS LibCrypto LibBase)
add_test(NAME LibTLS COMMAND TestLibTLS)
