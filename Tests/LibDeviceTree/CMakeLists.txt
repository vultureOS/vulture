#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibDeviceTree
    TestFlattenedDeviceTree.cpp)

target_link_libraries(TestLibDeviceTree PRIVATE LibTest LibDeviceTree LibBase)
add_test(NAME LibDeviceTree COMMAND TestLibDeviceTree)
