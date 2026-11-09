#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Runs a file of SQL against LibDatabase, and prints what comes back as the Mac's sqlite3 prints it, to be compared with it.
add_executable(dbshell main.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(dbshell PRIVATE LibDatabase LibBase)
