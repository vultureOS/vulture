#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibUnicode
    TestConformance.cpp
    TestProperties.cpp)

target_compile_definitions(TestLibUnicode PRIVATE VULTURE_UNICODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/Data/")
target_link_libraries(TestLibUnicode PRIVATE LibTest LibUnicode LibCompress LibBase)
add_test(NAME LibUnicode COMMAND TestLibUnicode)
