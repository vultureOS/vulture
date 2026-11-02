#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The system services' endpoints (logd, notifyd, configd, clipboardd), for the
# services and their clients, and the C interfaces macOS programs know
# them by: <os/log.h> and <notify.h>.
add_library(LibServices STATIC
    Notify.cpp
    OSLog.cpp)
vulture_compile_ipc(LibServices Audio.ipc Configuration.ipc Log.ipc Notify.ipc Pasteboard.ipc)
target_include_directories(LibServices PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LibServices PUBLIC LibIPC)
