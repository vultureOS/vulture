/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

// What the C library says for each locale, for Meta/oracle-test.sh to compare with a real Mac: the same program is built for both.

namespace {

char const* const s_locales[] = {
    "C", "POSIX", "en_US.UTF-8", "en_GB.UTF-8", "fr_FR.UTF-8", "de_DE.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8", "pt_BR.UTF-8", "nl_NL.UTF-8", "sv_SE.UTF-8", "pl_PL.UTF-8", "tr_TR.UTF-8", "ru_RU.UTF-8",
    "he_IL.UTF-8", "ja_JP.UTF-8", "ko_KR.UTF-8", "zh_CN.UTF-8", "hi_IN.UTF-8", "no_SUCH.UTF-8",
};

void begin(char const* locale, char const* what)
{
    printf("commands: BEGIN %s %s\n", locale, what);
}

void end(char const* locale, char const* what)
{
    printf("commands: END %s %s status=0\n", locale, what);
}

void say_number(char const* name, char value)
{
    printf("%s=%d\n", name, value == CHAR_MAX ? -1 : value);
}

void say_grouping(char const* name, char const* grouping)
{
    printf("%s=", name);
    for (; *grouping; ++grouping)
        printf("%d;", *grouping);
    printf("\n");
}

}

int main()
{
    struct tm moment {};
    moment.tm_year = 126;
    moment.tm_mon = 2;
    moment.tm_mday = 5;
    moment.tm_hour = 14;
    moment.tm_min = 7;
    moment.tm_sec = 9;
    moment.tm_wday = 4;
    moment.tm_yday = 63;
    // (Which zone this machine's in isn't what's being compared.)
    moment.tm_zone = const_cast<char*>("UTC");

    for (auto const* locale : s_locales) {
        char const* answer = setlocale(LC_ALL, locale);
        begin(locale, "name");
        printf("%s\n", answer ? answer : "(none)");
        end(locale, "name");
        if (!answer) {
            setlocale(LC_ALL, "C");
            continue;
        }

        auto const* conventions = localeconv();
        begin(locale, "numbers");
        printf("point=%s\nthousands=%s\n", conventions->decimal_point, conventions->thousands_sep);
        say_grouping("grouping", conventions->grouping);
        end(locale, "numbers");

        begin(locale, "money");
        printf("code=%s\nsymbol=%s\npoint=%s\nthousands=%s\n", conventions->int_curr_symbol, conventions->currency_symbol, conventions->mon_decimal_point, conventions->mon_thousands_sep);
        say_grouping("grouping", conventions->mon_grouping);
        printf("plus=%s\nminus=%s\n", conventions->positive_sign, conventions->negative_sign);
        say_number("digits", conventions->frac_digits);
        say_number("international digits", conventions->int_frac_digits);
        say_number("symbol first", conventions->p_cs_precedes);
        say_number("space", conventions->p_sep_by_space);
        say_number("sign", conventions->n_sign_posn);
        end(locale, "money");

        struct Format {
            char const* name;
            char const* format;
        };
        Format const formats[] = {
            { "weekday", "%a|%A" }, { "month", "%b|%B|%h" }, { "%x", "%x" }, { "%X", "%X" }, { "%c", "%c" }, { "am or pm", "%p" }, { "%r", "%r" }, { "date command", "%+" }, { "ISO", "%F %T %D %R" },
        };
        for (auto const& format : formats) {
            char text[256];
            begin(locale, format.name);
            size_t const length = strftime(text, sizeof(text), format.format, &moment);
            printf("%.*s\n", static_cast<int>(length), text);
            end(locale, format.name);
        }

        begin(locale, "langinfo");
        printf("%s|%s|%s|%s|%s|%s|%s\n", nl_langinfo(CODESET), nl_langinfo(D_T_FMT), nl_langinfo(D_FMT), nl_langinfo(T_FMT), nl_langinfo(T_FMT_AMPM), nl_langinfo(AM_STR), nl_langinfo(PM_STR));
        printf("%s|%s|%s|%s|%s\n", nl_langinfo(DAY_1), nl_langinfo(ABDAY_2), nl_langinfo(MON_3), nl_langinfo(ABMON_4), nl_langinfo(RADIXCHAR));
        end(locale, "langinfo");
    }

    // A category at a time.
    setlocale(LC_ALL, "C");
    setlocale(LC_TIME, "de_DE.UTF-8");
    begin("mixed", "name");
    printf("%s\n", setlocale(LC_ALL, nullptr));
    end("mixed", "name");
    setlocale(LC_ALL, "C");

    printf("commands: finished\n");
    return 0;
}
