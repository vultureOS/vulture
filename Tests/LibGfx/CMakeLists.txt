#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibGfx
    TestBlend.cpp
    TestFilter.cpp
    TestGeometry.cpp
    TestPainter.cpp
    TestRasterizer.cpp
    TestStroker.cpp)

target_link_libraries(TestLibGfx PRIVATE LibTest LibGfx LibBase)
add_test(NAME LibGfx COMMAND TestLibGfx)
