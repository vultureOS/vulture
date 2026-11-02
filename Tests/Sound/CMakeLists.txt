#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What comes out of the sound device, as QEMU records it: see Meta/sound-test.sh. The boot
# flag "soundtest" has vlaunchd run the jobs in usr/tests/sound.
vulture_add_program(sound-client SOURCES client.cpp DESTINATION usr/tests LIBRARIES LibAudio LibServices)
install(DIRECTORY Jobs/ DESTINATION usr/tests/sound)
