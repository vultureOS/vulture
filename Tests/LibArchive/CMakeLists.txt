#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibArchive
    TestTar.cpp)

target_link_libraries(TestLibArchive PRIVATE LibTest LibArchive LibBase)
add_test(NAME LibArchive COMMAND TestLibArchive)
