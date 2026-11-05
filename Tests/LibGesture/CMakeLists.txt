#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibGesture TestRecognizer.cpp)
target_link_libraries(TestLibGesture PRIVATE LibTest LibGesture LibBase)
add_test(NAME LibGesture COMMAND TestLibGesture)
