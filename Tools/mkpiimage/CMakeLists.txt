#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(mkpiimage
    main.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(mkpiimage PRIVATE LibBase)
