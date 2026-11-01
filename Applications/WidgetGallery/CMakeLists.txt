#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Every control of pranaUI, in a window: what the golden tests look at.
vulture_add_program(WidgetGallery
    SOURCES main.cpp
    DESTINATION Applications/WidgetGallery.app/Contents/MacOS
    LIBRARIES LibUI)
