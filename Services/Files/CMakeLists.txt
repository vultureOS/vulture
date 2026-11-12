#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Files
    SOURCES main.cpp Common.cpp Operations.cpp Views.cpp
    NAME "Files"
    IDENTIFIER com.vulture.Files
    ICON Files
    KIND "File Manager"
    CATEGORY utilities
    DESTINATION System/Library/CoreServices
    LIBRARIES LibDesktop LibDiskManagement)

# What Files does with things that could be anything: compressing, expanding, turning pictures.
vulture_add_program(FilesHelper SOURCES Helper.cpp DESTINATION usr/libexec LIBRARIES LibArchive LibCompress LibImage LibGfx)
