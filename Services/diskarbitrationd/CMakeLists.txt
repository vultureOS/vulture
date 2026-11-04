#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What happens when a disk comes or goes, and what those who may ask for about them.
vulture_add_program(diskarbitrationd SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibDiskManagement)
