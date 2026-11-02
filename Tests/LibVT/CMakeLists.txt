#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibVT TestTerminal.cpp)
target_link_libraries(TestLibVT PRIVATE LibTest LibVT LibBase)
add_test(NAME LibVT COMMAND TestLibVT)
