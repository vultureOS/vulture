#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# PranaWindowServer.
vulture_add_program(WindowServer
    SOURCES Compositor.cpp Cursors.cpp KeyboardLayout.cpp Screen.cpp Server.cpp Shell.cpp
    Spaces.cpp Theme.cpp WorkerPool.cpp main.cpp
    DESTINATION System/Library/CoreServices
    LIBRARIES LibAnimation LibGesture LibWindow LibText LibImage LibGfx LibJSON LibServices LibIPC LibCore)
