#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Disks
    SOURCES main.cpp
    NAME "Disks"
    IDENTIFIER com.vulture.Disks
    SANDBOX disks
    ICON Disks
    KIND "Volumes and Storage"
    LIBRARIES LibDesktop)
