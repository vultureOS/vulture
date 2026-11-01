#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The unified log.
vulture_add_program(logd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibServices LibCore)
