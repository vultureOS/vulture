#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Process 1.
vulture_add_program(vlaunchd
    SOURCES Job.cpp Log.cpp Supervisor.cpp TestRunner.cpp main.cpp
    DESTINATION sbin
    LIBRARIES LibCore LibJSON)
