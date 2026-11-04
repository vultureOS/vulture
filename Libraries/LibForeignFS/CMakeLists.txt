#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Built for the kernel and for the host tools alike.
add_library(LibForeignFS STATIC
    ExFAT.cpp
    FAT.cpp
    ISO9660.cpp
    Names.cpp
    Recognise.cpp
    SectorCache.cpp)
target_link_libraries(LibForeignFS PUBLIC LibUnicode)
