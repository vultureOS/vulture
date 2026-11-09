#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Pictures of what's in files, made in a process that can't reach any of them.
vulture_add_program(QuickViewHelper SOURCES helper.cpp DESTINATION usr/libexec)
vulture_add_quickview_plugin(Image DESTINATION System/Library/QuickView SOURCES ImagePlugin.cpp LIBRARIES LibImage LibGfx LibBase)
vulture_add_quickview_plugin(Text DESTINATION System/Library/QuickView SOURCES TextPlugin.cpp LIBRARIES LibText LibGfx LibBase)
