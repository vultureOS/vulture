#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(TestLibBase
    TestArray.cpp
    TestAtomic.cpp
    TestChecksum.cpp
    TestContainers.cpp
    TestErrorOr.cpp
    TestFastRandom.cpp
    TestFormat.cpp
    TestFunction.cpp
    TestIntrusiveList.cpp
    TestMath.cpp
    TestOptional.cpp
    TestQuickSort.cpp
    TestSmartPointers.cpp
    TestSpan.cpp
    TestStringView.cpp)

target_link_libraries(TestLibBase PRIVATE LibTest LibBase)
add_test(NAME LibBase COMMAND TestLibBase)
