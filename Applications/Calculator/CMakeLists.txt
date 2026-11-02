#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Calculator
    SOURCES main.cpp
    NAME "Calculator"
    IDENTIFIER com.vulture.Calculator
    ICON Calculator
    KIND "Arithmetic"
    LIBRARIES LibDesktop)
