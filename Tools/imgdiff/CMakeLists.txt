#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(ImgDiffCompare STATIC Compare.cpp)
target_include_directories(ImgDiffCompare PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ImgDiffCompare PUBLIC LibGfx LibBase)

add_executable(imgdiff
    main.cpp
    ${VULTURE_SOURCE_ROOT}/Libraries/LibTest/HostPlatform.cpp)
target_link_libraries(imgdiff PRIVATE ImgDiffCompare LibImage LibBase)
