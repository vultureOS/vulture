#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Built for the kernel and for the host tools (mkvfs, fsck.vfs) alike.
add_library(LibVultureFS STATIC
    Allocator.cpp
    BTree.cpp
    Check.cpp
    Volume.cpp)
