#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibBase
    TestArray.cpp
    TestErrorOr.cpp
    TestFormat.cpp
    TestOptional.cpp
    TestSpan.cpp
    TestStringView.cpp)

target_link_libraries(TestLibBase PRIVATE LibTest LibBase)
add_test(NAME LibBase COMMAND TestLibBase)
