#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# An object library so main() and the runtime hooks are always linked in.
add_library(LibTest OBJECT
    HostPlatform.cpp
    TestMain.cpp)
