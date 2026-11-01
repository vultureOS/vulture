#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The IPC endpoint compiler (see main.cpp); the userland build runs it.
add_executable(ipcc
    main.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(ipcc PRIVATE LibBase)
