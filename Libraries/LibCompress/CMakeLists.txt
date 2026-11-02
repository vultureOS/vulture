#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

if(VULTURE_PART STREQUAL "target")
    # The kernel only compresses pages.
    add_library(LibCompress STATIC
        PageCompression.cpp)
else()
    add_library(LibCompress STATIC
        Deflate.cpp
        PageCompression.cpp)
endif()
