#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Drawing: bitmaps, paths, coverage, strokes, gradients, blur. Built for the
# host too, for tools and tests.
add_library(LibGfx STATIC
    Bitmap.cpp
    Blend.cpp
    Filter.cpp
    Gradient.cpp
    Painter.cpp
    Path.cpp
    Rasterizer.cpp
    Stroker.cpp)
