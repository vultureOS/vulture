#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibCompress
    TestDeflate.cpp
    TestPageCompression.cpp)

target_link_libraries(TestLibCompress PRIVATE LibTest LibCompress LibBase)
add_test(NAME LibCompress COMMAND TestLibCompress)
