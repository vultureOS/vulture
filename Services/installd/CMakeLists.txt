#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Puts software on the system, for those who may. What the Software application asks.
vulture_add_program(installd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibPackage LibCore LibJSON)
