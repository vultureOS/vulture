#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_subdirectory(Monitor)
add_subdirectory(Archiver)
add_subdirectory(Calculator)
add_subdirectory(Console)
add_subdirectory(Disks)
add_subdirectory(Installer)
add_subdirectory(Launcher)
add_subdirectory(Viewer)
add_subdirectory(Software)
add_subdirectory(Settings)
add_subdirectory(Terminal)
add_subdirectory(TextEditor)
add_subdirectory(WidgetGallery)

# The icons, drawn by a host tool.
set(icons_directory "${CMAKE_CURRENT_BINARY_DIR}/Icons")
file(MAKE_DIRECTORY "${icons_directory}")
add_custom_command(
    OUTPUT "${icons_directory}/.stamp"
    # From nothing, so that a renamed icon does not leave its old self behind.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${icons_directory}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${icons_directory}"
    COMMAND ${VULTURE_HOST_TOOLS}/mkicons/mkicons "${icons_directory}"
    COMMAND ${CMAKE_COMMAND} -E touch "${icons_directory}/.stamp"
    DEPENDS ${VULTURE_HOST_TOOLS}/mkicons/mkicons
    COMMENT "Drawing the icons")
add_custom_target(icons ALL DEPENDS "${icons_directory}/.stamp")
install(DIRECTORY "${icons_directory}/" DESTINATION System/Library/Icons PATTERN ".stamp" EXCLUDE PATTERN "*.png" EXCLUDE)
# Drawn along with them: a picture to open.
install(FILES "${icons_directory}/Dunes.png" DESTINATION System/Library/Pictures)
