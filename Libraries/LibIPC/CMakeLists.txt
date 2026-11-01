#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Messages over ports, the endpoints ipcc generates use, and the bootstrap
# (name) service.
add_library(LibIPC STATIC
    Bootstrap.cpp
    Listener.cpp
    Message.cpp)
vulture_compile_ipc(LibIPC Bootstrap.ipc)
target_link_libraries(LibIPC PUBLIC LibCore)
