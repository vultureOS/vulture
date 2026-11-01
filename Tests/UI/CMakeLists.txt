#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Golden screenshots: see ninja test-ui. The boot flag "uitest" has vlaunchd run
# the jobs in usr/tests/ui.
vulture_add_program(ui-scene SOURCES scene.cpp DESTINATION usr/tests LIBRARIES LibWindow LibText LibGfx LibIPC LibCore)
install(DIRECTORY Jobs/ DESTINATION usr/tests/ui)
install(DIRECTORY GalleryJobs/ DESTINATION usr/tests/gallery)
