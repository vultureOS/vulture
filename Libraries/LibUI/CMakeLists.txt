#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# pranaUI. What talks to the window server (Application) is only built for the system;
# the rest also for the host, so its tests run with no server at all.
set(LibUI_SOURCES
    Accessibility.cpp
    Appearance.cpp
    ConstraintSolver.cpp
    Containers.cpp
    Controls.cpp
    Graphics.cpp
    Layout.cpp
    Menu.cpp
    Motion.cpp
    Panels.cpp
    Platform.cpp
    ScrollView.cpp
    TableView.cpp
    TextField.cpp
    TextStorage.cpp
    TextView.cpp
    View.cpp
    Window.cpp)

if(VULTURE_PART STREQUAL "userland")
    list(APPEND LibUI_SOURCES Application.cpp)
endif()

add_library(LibUI STATIC ${LibUI_SOURCES})
target_link_libraries(LibUI PUBLIC LibAnimation LibText LibGfx LibJSON LibBase)
if(VULTURE_PART STREQUAL "userland")
    target_link_libraries(LibUI PUBLIC LibWindow LibServices LibIPC LibCore)
endif()
