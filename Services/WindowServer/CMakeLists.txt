#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# PranaWindowServer.
vulture_add_program(WindowServer
    SOURCES Compositor.cpp Screen.cpp Server.cpp Theme.cpp WorkerPool.cpp main.cpp
    DESTINATION System/Library/CoreServices
    LIBRARIES LibWindow LibText LibImage LibGfx LibJSON LibServices LibIPC LibCore)
