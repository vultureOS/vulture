#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The system configuration: the machine's names.
vulture_add_program(configd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibServices LibCore LibJSON)
