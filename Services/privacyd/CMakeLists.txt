#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Keeps what people have said applications may have, and asks them when it doesn't know.
vulture_add_program(privacyd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibServices LibCore LibJSON)
