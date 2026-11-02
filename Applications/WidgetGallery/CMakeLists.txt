#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Every control of pranaUI, in a window: what the golden tests look at.
vulture_add_application(WidgetGallery
    SOURCES main.cpp
    NAME "Widget Gallery"
    IDENTIFIER com.vulture.WidgetGallery
    ICON WidgetGallery
    KIND "Control Catalog"
    LIBRARIES LibDesktop)
