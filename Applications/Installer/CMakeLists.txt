#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Installer
    SOURCES main.cpp
    NAME "Installer"
    IDENTIFIER com.vulture.Installer
    SANDBOX installer
    ICON Installer
    KIND "Put vultureOS on a Disk"
    CATEGORY utilities
    LIBRARIES LibDesktop LibPackage LibJSON)
