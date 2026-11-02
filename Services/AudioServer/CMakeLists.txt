#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The one program with the sound device open, adding together what everyone wants heard.
vulture_add_program(AudioServer SOURCES main.cpp DESTINATION usr/libexec LIBRARIES LibAudio LibServices LibCore LibJSON)
