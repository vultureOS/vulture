#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibPackage TestPackage.cpp)
target_link_libraries(TestLibPackage PRIVATE LibTest LibPackage LibJSON LibCrypto LibBase)
add_test(NAME LibPackage COMMAND TestLibPackage)
