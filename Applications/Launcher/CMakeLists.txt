#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Launcher
    SOURCES main.cpp
    NAME "Launcher"
    IDENTIFIER com.vulture.launcher
    ICON Launcher
    KIND "All Applications"
    LIBRARIES LibDesktop)
