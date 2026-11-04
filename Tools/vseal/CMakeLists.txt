#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(vseal
    main.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_include_directories(vseal PRIVATE ${VULTURE_SOURCE_ROOT}/Libraries)
target_link_libraries(vseal PRIVATE LibArchive LibCrypto LibVultureFS LibBase)
