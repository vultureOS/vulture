#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The host side of vultureFS: the same LibVultureFS the kernel mounts.
add_library(VFSImage STATIC Image.cpp)
target_link_libraries(VFSImage PUBLIC LibVultureFS LibBase)

add_executable(mkvfs
    mkvfs.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(mkvfs PRIVATE VFSImage LibArchive LibVultureFS LibBase)

add_executable(fsck.vfs
    fsck.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(fsck.vfs PRIVATE VFSImage LibVultureFS LibBase)
