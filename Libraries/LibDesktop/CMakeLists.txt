#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What the programs of the desktop share: applications and how to open things, icons,
# what the window server says of the session, preferences, and notifications.
add_library(LibDesktop STATIC
    Desktop.cpp)
vulture_compile_ipc(LibDesktop Finder.ipc Notifications.ipc Search.ipc Session.ipc)
target_link_libraries(LibDesktop PUBLIC LibUI LibImage LibJSON LibIPC LibCore)
target_include_directories(LibDesktop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
