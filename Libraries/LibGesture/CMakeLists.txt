#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What fingers mean. Built for the host too, for tests.
add_library(LibGesture STATIC
    Recognizer.cpp)
target_link_libraries(LibGesture PUBLIC LibBase)
