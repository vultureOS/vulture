#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Makes and takes away people's accounts, for Settings.
vulture_add_program(directoryd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibDesktop LibCore)
