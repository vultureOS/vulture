#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibAnimation TestCurve.cpp TestAnimator.cpp)
target_link_libraries(TestLibAnimation PRIVATE LibTest LibAnimation LibBase)
add_test(NAME LibAnimation COMMAND TestLibAnimation)
