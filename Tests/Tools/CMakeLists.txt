#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestImgDiff TestImgDiff.cpp)
target_compile_definitions(TestImgDiff PRIVATE "GOLDEN_DIRECTORY=\"${VULTURE_SOURCE_ROOT}/Tests/UI/Golden\"")
target_link_libraries(TestImgDiff PRIVATE LibTest ImgDiffCompare LibImage LibGfx LibBase)
add_test(NAME ImgDiff COMMAND TestImgDiff)
