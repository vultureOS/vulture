#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The parts of pranaUI that need no window server.
add_executable(TestLibUI TestConstraintSolver.cpp ../../Libraries/LibUI/ConstraintSolver.cpp)
target_include_directories(TestLibUI PRIVATE ${CMAKE_SOURCE_DIR}/Libraries)
target_link_libraries(TestLibUI PRIVATE LibTest LibBase)
add_test(NAME LibUI COMMAND TestLibUI)
