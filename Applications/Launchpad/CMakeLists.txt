#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Launchpad
    SOURCES main.cpp
    NAME "Launchpad"
    IDENTIFIER com.vulture.launchpad
    ICON Launchpad
    LIBRARIES LibDesktop)
