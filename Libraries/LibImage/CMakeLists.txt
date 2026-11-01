#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(LibImage STATIC
    JPEG.cpp
    PNG.cpp)
target_link_libraries(LibImage PUBLIC LibCompress LibGfx)
