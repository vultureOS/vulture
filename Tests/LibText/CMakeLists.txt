#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibText
    TestText.cpp)

target_compile_definitions(TestLibText PRIVATE
    VULTURE_FONT_DIRECTORY="${VULTURE_SOURCE_ROOT}/Base/System/Library/Fonts/"
    VULTURE_TEXT_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data/")
target_link_libraries(TestLibText PRIVATE LibTest LibText LibGfx LibBase)
add_test(NAME LibText COMMAND TestLibText)

# What this system's shaper does with some text in some font: Meta/oracle-test.sh compares it with what Core Text does.
add_executable(shape-print ShapePrint.cpp ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(shape-print PRIVATE LibText LibGfx LibBase)
