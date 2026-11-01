#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Motion: curves, springs and what runs them. Built for the host too, for tests.
add_library(LibAnimation STATIC
    Animator.cpp
    Curve.cpp)
target_link_libraries(LibAnimation PUBLIC LibBase)
