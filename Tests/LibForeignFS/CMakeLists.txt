#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibForeignFS
    TestForeignFS.cpp)
target_link_libraries(TestLibForeignFS PRIVATE LibTest LibForeignFS LibUnicode LibBase)
add_test(NAME LibForeignFS COMMAND TestLibForeignFS)

# What Meta/media-test.sh has a real Mac's tools look at, and the other way about.
add_executable(foreignfs-exercise
    exercise.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(foreignfs-exercise PRIVATE LibForeignFS LibUnicode LibBase)
