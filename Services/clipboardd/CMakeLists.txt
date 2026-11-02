#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The pasteboard, for copy and paste.
vulture_add_program(pboard SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibServices LibCore)
