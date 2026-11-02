#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(SystemSettings
    SOURCES main.cpp
    NAME "System Settings"
    IDENTIFIER com.vulture.SystemSettings
    ICON SystemSettings
    LIBRARIES LibDesktop)
