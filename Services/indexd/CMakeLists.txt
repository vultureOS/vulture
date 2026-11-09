#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The index behind Search, Spotlight's model: a database of what's where, kept up by the file system's own log of changes.
vulture_add_program(indexd SOURCES main.cpp Index.cpp Importers.cpp QueryLanguage.cpp DESTINATION usr/libexec LIBRARIES LibDesktop LibDatabase LibUnicode)
