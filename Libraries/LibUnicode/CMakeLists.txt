#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# What Unicode says about text. Its tables are made by Meta/generate-unicode.py, and each topic is its own object, so that what links this only takes what it uses.
add_library(LibUnicode STATIC
    Bidi.cpp
    CaseMapping.cpp
    LineBreak.cpp
    Normalization.cpp
    Properties.cpp
    Segmentation.cpp
    UTF8.cpp
    Generated/Bidi.cpp
    Generated/Booleans.cpp
    Generated/CaseFolding.cpp
    Generated/CaseMapping.cpp
    Generated/CombiningClass.cpp
    Generated/Digits.cpp
    Generated/EastAsianWidth.cpp
    Generated/GeneralCategory.cpp
    Generated/LineBreak.cpp
    Generated/Normalization.cpp
    Generated/ScriptNames.cpp
    Generated/Scripts.cpp
    Generated/Segmentation.cpp
    Generated/Shaping.cpp)
target_link_libraries(LibUnicode PUBLIC LibBase)
