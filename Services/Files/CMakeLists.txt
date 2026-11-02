#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Finder
    SOURCES main.cpp
    NAME "Finder"
    IDENTIFIER com.vulture.Finder
    ICON Finder
    DESTINATION System/Library/CoreServices
    LIBRARIES LibDesktop)
