#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Files
    SOURCES main.cpp
    NAME "Files"
    IDENTIFIER com.vulture.Files
    ICON Files
    KIND "File Manager"
    DESTINATION System/Library/CoreServices
    LIBRARIES LibDesktop)
