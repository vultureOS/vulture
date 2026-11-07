#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(Archiver
    SOURCES main.cpp
    NAME "Archiver"
    IDENTIFIER com.vulture.Archiver
    SANDBOX document-editor
    ICON Archiver
    KIND "Compress and Expand"
    CATEGORY utilities
    EXTENSIONS tar gz tgz
    LIBRARIES LibDesktop LibArchive LibCompress)
