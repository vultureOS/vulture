#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Sound, as numbers: the system's sounds, made rather than recorded; mixing; WAV files.
# Nothing here touches a device, so it all runs on the host too, for tests.
add_library(LibAudio STATIC
    Mixer.cpp
    Sounds.cpp
    Wav.cpp)
target_link_libraries(LibAudio PUBLIC LibBase)
