#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The Sound menu extra: a program of its own in the menu bar, as a Mac's are.
vulture_add_program(SoundMenu SOURCES main.cpp DESTINATION System/Library/CoreServices LIBRARIES LibDesktop)
