#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Viewer
    SOURCES main.cpp
    NAME "Viewer"
    IDENTIFIER com.vulture.Viewer
    ICON Viewer
    KIND "Pictures and Images"
    EXTENSIONS png jpg jpeg
    LIBRARIES LibDesktop)
