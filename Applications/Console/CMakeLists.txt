#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Console
    SOURCES main.cpp
    NAME "Console"
    IDENTIFIER com.vulture.Console
    SANDBOX console
    ICON Console
    KIND "System Log"
    CATEGORY utilities
    LIBRARIES LibDesktop LibServices)
