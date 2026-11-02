#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Gets each network interface an address, by DHCP, and keeps it.
vulture_add_program(ipconfigd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibServices LibCore)
