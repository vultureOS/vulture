#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# An embedded SQL database, for what services and applications keep. Built for the host too, to be tested against the Mac's
# own SQLite.
add_library(LibDatabase STATIC
    BTree.cpp
    Database.cpp
    Pager.cpp
    SQL.cpp
    Value.cpp)

target_link_libraries(LibDatabase PUBLIC LibBase)
