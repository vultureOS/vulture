#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What applications keep apart from their windows, so that it can run here.
add_executable(TestApplications TestCalculator.cpp TestConsole.cpp)
target_include_directories(TestApplications PRIVATE "${VULTURE_SOURCE_ROOT}/Applications")
target_link_libraries(TestApplications PRIVATE LibTest LibBase)
add_test(NAME Applications COMMAND TestApplications)
