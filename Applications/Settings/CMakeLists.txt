#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Settings
    SOURCES main.cpp
    NAME "Settings"
    IDENTIFIER com.vulture.Settings
    ICON Settings
    KIND "System Configuration"
    CATEGORY utilities
    LIBRARIES LibDesktop LibDiskManagement LibLocale)
