/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What's been read from the current locale's LC_TIME, for strftime() and nl_langinfo() (locale.cpp).

namespace LibC {

struct TimeNames {
    char const* short_months[12];
    char const* months[12];
    char const* short_days[7];
    char const* days[7];
    char const* time_format;
    char const* date_format;
    char const* date_time_format;
    char const* am;
    char const* pm;
    char const* date_command_format;
    char const* standalone_months[12];
    char const* date_order;
    char const* am_pm_format;
};

TimeNames const& time_names();

}
