#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibLocale
    TestLocale.cpp)

target_compile_definitions(TestLibLocale PRIVATE VULTURE_LOCALE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/Data/")
target_link_libraries(TestLibLocale PRIVATE LibTest LibLocale LibCompress LibBase)
add_test(NAME LibLocale COMMAND TestLibLocale)
