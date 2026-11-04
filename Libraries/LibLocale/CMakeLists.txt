#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# How the world writes numbers and dates, and sorts words. Its tables are made by Meta/generate-locale.py.
add_library(LibLocale STATIC
    Collation.cpp
    Dates.cpp
    Locale.cpp
    Numbers.cpp
    Generated/CollationRoot.cpp
    Generated/CollationTailoring.cpp
    Generated/Locales.cpp)
target_link_libraries(LibLocale PUBLIC LibUnicode LibBase)
