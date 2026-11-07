#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Calculator
    SOURCES main.cpp
    NAME "Calculator"
    IDENTIFIER com.vulture.Calculator
    SANDBOX calculator
    ICON Calculator
    KIND "Arithmetic"
    CATEGORY utilities
    LIBRARIES LibDesktop)
