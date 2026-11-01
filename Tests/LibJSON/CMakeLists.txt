#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibJSON
    TestJSON.cpp)

target_link_libraries(TestLibJSON PRIVATE LibTest LibJSON LibBase)
add_test(NAME LibJSON COMMAND TestLibJSON)
