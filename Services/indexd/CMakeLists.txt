#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The index behind Search.
vulture_add_program(indexd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibDesktop)
