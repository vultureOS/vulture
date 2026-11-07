#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Who is using this computer: asks, and starts their session.
vulture_add_program(LoginWindow SOURCES main.cpp Setup.cpp Views.cpp DESTINATION System/Library/CoreServices LIBRARIES LibDesktop LibCore LibLocale LibJSON)
