#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibHTTP TestHTTP.cpp)
target_link_libraries(TestLibHTTP PRIVATE LibTest LibHTTP LibBase)
add_test(NAME LibHTTP COMMAND TestLibHTTP)
