#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Software
    SOURCES main.cpp
    NAME "Software"
    IDENTIFIER com.vulture.Software
    ICON Software
    KIND "Applications"
    LIBRARIES LibDesktop)
