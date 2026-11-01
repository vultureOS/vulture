#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The menu bar and what hangs off it: the system's menu, the application's menus, the
# clock, Control Center, Notification Center, Spotlight.
vulture_add_program(SystemUIServer SOURCES main.cpp DESTINATION System/Library/CoreServices LIBRARIES LibDesktop)
