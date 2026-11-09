#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibDatabase TestDatabase.cpp)
target_link_libraries(TestLibDatabase PRIVATE LibTest LibDatabase LibBase)
add_test(NAME LibDatabase COMMAND TestLibDatabase)

# The same statements on the Mac's own SQLite, and the answers compared.
add_test(NAME LibDatabaseOracle COMMAND ${VULTURE_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/oracle.py $<TARGET_FILE:dbshell> ${CMAKE_CURRENT_SOURCE_DIR}/oracle.sql)
