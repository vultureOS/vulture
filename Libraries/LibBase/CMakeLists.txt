#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(LibBase STATIC
    Format.cpp)

# Hosted code gets these from the C library.
if(NOT VULTURE_PART STREQUAL "host")
    add_library(LibBaseFreestanding STATIC
        Freestanding/Memory.cpp)
endif()
