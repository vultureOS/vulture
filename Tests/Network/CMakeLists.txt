#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The whole of networking, against servers on the build machine: see Meta/network-test.sh. The
# boot flag "nettest" has vlaunchd run the jobs in usr/tests/network.
install(DIRECTORY Jobs/ DESTINATION usr/tests/network)
install(PROGRAMS network-client.sh network-gui-setup.sh DESTINATION usr/tests)
# The same, with the desktop: Software, against that repository.
install(DIRECTORY ../UI/DesktopJobs/ DESTINATION usr/tests/network-gui)
install(DIRECTORY GuiJobs/ DESTINATION usr/tests/network-gui)

# An application to put in a package, kept apart from the system so that it isn't on it until
# it has been installed.
vulture_add_application(Hello
    SOURCES HelloApp/main.cpp
    NAME "Hello"
    IDENTIFIER com.vulture.test.Hello
    ICON Software
    KIND "A test package"
    DESTINATION usr/tests/package-payload/Applications
    LIBRARIES LibDesktop)
