#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Kernel code that does not touch hardware is compiled for the host and
# tested here directly.
add_executable(TestKernel
    TestBootArguments.cpp
    TestBuddyAllocator.cpp
    ${VULTURE_SOURCE_ROOT}/Kernel/BootArguments.cpp
    ${VULTURE_SOURCE_ROOT}/Kernel/Memory/BuddyAllocator.cpp)

target_link_libraries(TestKernel PRIVATE LibTest LibBase)
add_test(NAME Kernel COMMAND TestKernel)
