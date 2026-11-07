#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Monitor
    SOURCES main.cpp
    NAME "Monitor"
    IDENTIFIER com.vulture.Monitor
    ICON Monitor
    KIND "Processes and Activity"
    CATEGORY utilities
    LIBRARIES LibDesktop)
