#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

vulture_add_application(TextEditor
    SOURCES main.cpp
    NAME "Text Editor"
    IDENTIFIER com.vulture.TextEditor
    SANDBOX document-editor
    ICON TextEditor
    KIND "Plain Text and Notes"
    CATEGORY productivity
    EXTENSIONS txt text md log json cpp h conf plist
    SERVICES "New Text Editor Window Containing Selection|newWindowContainingSelection"
    LIBRARIES LibDesktop)
