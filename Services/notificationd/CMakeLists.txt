#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Keeps notifications, and tells whoever shows them.
vulture_add_program(notificationd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibDesktop)
