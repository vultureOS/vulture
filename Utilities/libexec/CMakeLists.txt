#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Makes a disk what this was started from, with the code that does that on the machine that builds it.
set(vfs_tools ${VULTURE_SOURCE_ROOT}/Tools/vfs)
vulture_add_program(vinstall
    SOURCES vinstall.cpp ${vfs_tools}/FAT32.cpp ${vfs_tools}/Image.cpp ${vfs_tools}/Populate.cpp
    DESTINATION usr/libexec
    LIBRARIES LibArchive LibVultureFS)
target_include_directories(vinstall PRIVATE ${vfs_tools})
