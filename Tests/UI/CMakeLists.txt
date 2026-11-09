#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Golden screenshots: see ninja test-ui. The boot flag "uitest" has vlaunchd run
# the jobs in usr/tests/ui.
vulture_add_program(ui-scene SOURCES scene.cpp DESTINATION usr/tests LIBRARIES LibWindow LibText LibGfx LibIPC LibCore)
vulture_add_program(ui-languages SOURCES languages.cpp DESTINATION usr/tests LIBRARIES LibUI)
set_target_properties(ui-languages PROPERTIES OUTPUT_NAME languages)
install(DIRECTORY Jobs/ DESTINATION usr/tests/ui)
install(DIRECTORY GalleryJobs/ DESTINATION usr/tests/gallery)
install(DIRECTORY DesktopJobs/ DESTINATION usr/tests/desktop)
install(DIRECTORY BudgetJobs/ DESTINATION usr/tests/budget-jobs)
vulture_add_program(budgets SOURCES budgets.cpp DESTINATION usr/tests)
vulture_add_program(search-budget SOURCES search-budget.cpp DESTINATION usr/tests LIBRARIES LibDesktop)
