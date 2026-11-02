#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set(cursors ${VULTURE_SOURCE_ROOT}/Services/WindowServer/Cursors.cpp)

add_executable(TestCursors TestCursors.cpp ${cursors})
target_include_directories(TestCursors PRIVATE ${VULTURE_SOURCE_ROOT}/Services/WindowServer)
target_link_libraries(TestCursors PRIVATE LibTest LibGfx LibBase)
add_test(NAME Cursors COMMAND TestCursors)

# Every shape on paper, for Meta/ui-test.sh to compare and for a person to look at.
add_executable(cursor-sheet cursor-sheet.cpp ${cursors} ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_include_directories(cursor-sheet PRIVATE ${VULTURE_SOURCE_ROOT}/Services/WindowServer)
target_link_libraries(cursor-sheet PRIVATE LibImage LibGfx LibBase)
