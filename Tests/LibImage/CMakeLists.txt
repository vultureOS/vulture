#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibImage
    TestImage.cpp)

target_compile_definitions(TestLibImage PRIVATE VULTURE_IMAGE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data/")
target_link_libraries(TestLibImage PRIVATE LibTest LibImage LibBase)
add_test(NAME LibImage COMMAND TestLibImage)
