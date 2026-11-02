#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# A terminal: what a stream of bytes from a program does to a screenful of cells.
# Nothing here draws. Built for the host too, for tests.
add_library(LibVT STATIC Terminal.cpp)
target_link_libraries(LibVT PUBLIC LibBase)
