#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# HostPlatform.cpp supplies the runtime hooks (VERIFY failures, operator
# new) that host code built without the C++ standard library needs.
add_executable(mkarchive
    main.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(mkarchive PRIVATE LibArchive LibBase)
