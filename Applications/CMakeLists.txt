#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_subdirectory(Launchpad)
add_subdirectory(SystemSettings)
add_subdirectory(WidgetGallery)

# The icons, drawn by a host tool.
set(icons_directory "${CMAKE_CURRENT_BINARY_DIR}/Icons")
file(MAKE_DIRECTORY "${icons_directory}")
add_custom_command(
    OUTPUT "${icons_directory}/.stamp"
    COMMAND ${VULTURE_HOST_TOOLS}/mkicons/mkicons "${icons_directory}"
    COMMAND ${CMAKE_COMMAND} -E touch "${icons_directory}/.stamp"
    DEPENDS ${VULTURE_HOST_TOOLS}/mkicons/mkicons
    COMMENT "Drawing the icons")
add_custom_target(icons ALL DEPENDS "${icons_directory}/.stamp")
install(DIRECTORY "${icons_directory}/" DESTINATION System/Library/Icons PATTERN ".stamp" EXCLUDE)
