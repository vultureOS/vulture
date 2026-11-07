#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Terminal
    SOURCES main.cpp
    NAME "Terminal"
    IDENTIFIER com.vulture.Terminal
    ICON Terminal
    KIND "Command Line"
    CATEGORY utilities
    LIBRARIES LibDesktop LibVT)
