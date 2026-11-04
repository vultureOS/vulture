/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LocaleFiles.h"
#include <langinfo.h>
#include <locale.h>
#include <string.h>

extern "C" {

char* nl_langinfo(nl_item item)
{
    auto const& names = LibC::time_names();
    auto const answer = [](char const* text) { return const_cast<char*>(text); };
    if (item >= DAY_1 && item <= DAY_7)
        return answer(names.days[item - DAY_1]);
    if (item >= ABDAY_1 && item <= ABDAY_7)
        return answer(names.short_days[item - ABDAY_1]);
    if (item >= MON_1 && item <= MON_12)
        return answer(names.months[item - MON_1]);
    if (item >= ABMON_1 && item <= ABMON_12)
        return answer(names.short_months[item - ABMON_1]);
    switch (item) {
    case CODESET: {
        // (Text is UTF-8 whichever it is, but a program asking is deciding what to send to whoever's reading it, as on a Mac.)
        char const* name = setlocale(LC_CTYPE, nullptr);
        return answer(strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0 ? "US-ASCII" : "UTF-8");
    }
    case D_T_FMT:
        return answer(names.date_time_format);
    case D_FMT:
        return answer(names.date_format);
    case T_FMT:
        return answer(names.time_format);
    // (A locale that doesn't use them hasn't got them: and what's asked for by name gets C's.)
    case T_FMT_AMPM:
        return answer(*names.am_pm_format ? names.am_pm_format : "%I:%M:%S %p");
    case AM_STR:
        return answer(*names.am ? names.am : "AM");
    case PM_STR:
        return answer(*names.pm ? names.pm : "PM");
    case RADIXCHAR:
        return localeconv()->decimal_point;
    case THOUSEP:
        return localeconv()->thousands_sep;
    case YESEXPR:
        return answer("^[yY]");
    case NOEXPR:
        return answer("^[nN]");
    case D_MD_ORDER:
        return answer(names.date_order);
    default:
        return answer("");
    }
}
}
