#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# pranaUI, with a platform of the test's own in place of the window server.
add_library(LibUITestSupport STATIC Catalog.cpp Render.cpp TestPlatform.cpp)
target_compile_definitions(LibUITestSupport PUBLIC
    VULTURE_LIBRARY_DIRECTORY="${VULTURE_SOURCE_ROOT}/Base/System/Library"
    VULTURE_MEASURED_DIRECTORY="${VULTURE_SOURCE_ROOT}/Tests/UI/Measured")
target_link_libraries(LibUITestSupport PUBLIC LibUI LibImage)

add_executable(TestLibUI
    TestConstraintSolver.cpp
    TestControls.cpp
    TestMetrics.cpp)
target_link_libraries(TestLibUI PRIVATE LibTest LibUITestSupport)
add_test(NAME LibUI COMMAND TestLibUI)

# For calibrating: see Meta/Calibration/compare.py.
add_executable(ui-catalog ui-catalog.cpp ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(ui-catalog PRIVATE LibUITestSupport)
