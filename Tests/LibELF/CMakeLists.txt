#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibELF
    TestDemangle.cpp
    TestImage.cpp)

target_link_libraries(TestLibELF PRIVATE LibTest LibELF LibBase)
add_test(NAME LibELF COMMAND TestLibELF)
