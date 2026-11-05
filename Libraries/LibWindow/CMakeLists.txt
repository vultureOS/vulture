#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Windows, for programs: the link to PranaWindowServer.
add_library(LibWindow STATIC
    Window.cpp)
vulture_compile_ipc(LibWindow WindowServer.ipc)
target_link_libraries(LibWindow PUBLIC LibIPC LibCore LibGfx LibAnimation)
