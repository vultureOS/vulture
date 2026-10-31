#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibVultureFS
    TestVultureFS.cpp)

target_link_libraries(TestLibVultureFS PRIVATE LibTest LibVultureFS LibBase)
add_test(NAME LibVultureFS COMMAND TestLibVultureFS)
