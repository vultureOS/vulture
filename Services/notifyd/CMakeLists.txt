#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Notifications by name (notify(3)).
vulture_add_program(notifyd SOURCES main.cpp DESTINATION usr/sbin LIBRARIES LibServices LibCore)
