#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(LibDiskManagement STATIC Disks.cpp)
target_link_libraries(LibDiskManagement PUBLIC LibForeignFS LibBase)
