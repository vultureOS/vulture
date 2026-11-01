#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Fonts: parsing, shaping, glyph images. Built for the host too, for tests.
add_library(LibText STATIC
    Font.cpp
    Typeface.cpp)
target_link_libraries(LibText PUBLIC LibGfx)
