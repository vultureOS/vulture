#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibAudio TestAudio.cpp)
target_link_libraries(TestLibAudio PRIVATE LibTest LibAudio LibBase)
add_test(NAME LibAudio COMMAND TestLibAudio)
