#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibACPI
    TestAML.cpp)

target_link_libraries(TestLibACPI PRIVATE LibTest LibACPI LibBase)
add_test(NAME LibACPI COMMAND TestLibACPI)
