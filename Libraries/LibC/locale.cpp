/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Lock.h"
#include "LocaleFiles.h"
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int category_count = 7;
constexpr size_t longest_name = 48;

char const* const s_category_names[category_count] = { "LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME", "LC_MESSAGES" };

char s_names[category_count][longest_name] = { "C", "C", "C", "C", "C", "C", "C" };
// What setlocale(LC_ALL, nullptr) says, when they're not all the same: C/UTF-8/C/C/C/C.
char s_all_name[category_count * longest_name];

LibC::Lock s_lock;

// A locale's file: a line for each thing, in an order that's known. Kept for good, as what's been handed out might still be in use.
struct LoadedFile {
    char* text { nullptr };
    char const* lines[64] {};
    size_t count { 0 };
};

LoadedFile load(char const* locale, char const* file_name)
{
    LoadedFile loaded;
    char path[128 + longest_name];
    snprintf(path, sizeof(path), "/usr/share/locale/%s/%s", locale, file_name);
    int const fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return loaded;
    struct stat information {};
    if (fstat(fd, &information) != 0 || information.st_size > 16 * 1024) {
        close(fd);
        return loaded;
    }
    size_t const size = static_cast<size_t>(information.st_size);
    char* text = static_cast<char*>(malloc(size + 1));
    if (!text) {
        close(fd);
        return loaded;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t const one = read(fd, text + got, size - got);
        if (one <= 0)
            break;
        got += static_cast<size_t>(one);
    }
    close(fd);
    text[got] = 0;
    loaded.text = text;
    char* cursor = text;
    while (*cursor && loaded.count < sizeof(loaded.lines) / sizeof(loaded.lines[0])) {
        loaded.lines[loaded.count++] = cursor;
        char* end = strchr(cursor, '\n');
        if (!end)
            break;
        *end = 0;
        cursor = end + 1;
    }
    return loaded;
}

char const* line_of(LoadedFile const& file, size_t index, char const* fallback)
{
    return index < file.count ? file.lines[index] : fallback;
}

// ---- Time.

LibC::TimeNames const s_c_time = {
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
    { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" },
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
    "%H:%M:%S",
    "%m/%d/%y",
    "%a %b %e %H:%M:%S %Y",
    "AM",
    "PM",
    "%a %b %e %H:%M:%S %Z %Y",
    { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" },
    "md",
    "%I:%M:%S %p",
};

LibC::TimeNames s_time = s_c_time;

void set_time_from(char const* locale)
{
    s_time = s_c_time;
    if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
        return;
    auto const file = load(locale, "LC_TIME");
    if (!file.text)
        return;
    size_t line = 0;
    for (int i = 0; i < 12; ++i)
        s_time.short_months[i] = line_of(file, line++, s_c_time.short_months[i]);
    for (int i = 0; i < 12; ++i)
        s_time.months[i] = line_of(file, line++, s_c_time.months[i]);
    for (int i = 0; i < 7; ++i)
        s_time.short_days[i] = line_of(file, line++, s_c_time.short_days[i]);
    for (int i = 0; i < 7; ++i)
        s_time.days[i] = line_of(file, line++, s_c_time.days[i]);
    s_time.time_format = line_of(file, line++, s_c_time.time_format);
    s_time.date_format = line_of(file, line++, s_c_time.date_format);
    s_time.date_time_format = line_of(file, line++, s_c_time.date_time_format);
    s_time.am = line_of(file, line++, s_c_time.am);
    s_time.pm = line_of(file, line++, s_c_time.pm);
    s_time.date_command_format = line_of(file, line++, s_c_time.date_command_format);
    for (int i = 0; i < 12; ++i)
        s_time.standalone_months[i] = line_of(file, line++, s_time.months[i]);
    s_time.date_order = line_of(file, line++, s_c_time.date_order);
    s_time.am_pm_format = line_of(file, line++, s_c_time.am_pm_format);
}

// ---- Numbers and money.

char s_point[] = ".";
char s_empty[] = "";

lconv s_conventions = {
    s_point, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty,
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX
};

// "3;2" is three in the first group, then two. One byte for each.
char* grouping_from(char const* text)
{
    char* out = static_cast<char*>(malloc(strlen(text) + 2));
    if (!out)
        return s_empty;
    size_t at = 0;
    while (*text) {
        if (*text == ';') {
            ++text;
            continue;
        }
        int value = 0;
        while (*text >= '0' && *text <= '9')
            value = value * 10 + (*text++ - '0');
        out[at++] = static_cast<char>(value);
        if (*text && *text != ';')
            ++text;
    }
    out[at] = 0;
    return out;
}

char number_from(LoadedFile const& file, size_t index)
{
    char const* text = line_of(file, index, "");
    return *text ? static_cast<char>(atoi(text)) : CHAR_MAX;
}

void set_numbers_from(char const* locale)
{
    s_conventions.decimal_point = s_point;
    s_conventions.thousands_sep = s_empty;
    s_conventions.grouping = s_empty;
    if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
        return;
    auto const file = load(locale, "LC_NUMERIC");
    if (!file.text)
        return;
    s_conventions.decimal_point = const_cast<char*>(line_of(file, 0, "."));
    s_conventions.thousands_sep = const_cast<char*>(line_of(file, 1, ""));
    s_conventions.grouping = grouping_from(line_of(file, 2, ""));
}

void set_money_from(char const* locale)
{
    s_conventions.int_curr_symbol = s_empty;
    s_conventions.currency_symbol = s_empty;
    s_conventions.mon_decimal_point = s_empty;
    s_conventions.mon_thousands_sep = s_empty;
    s_conventions.mon_grouping = s_empty;
    s_conventions.positive_sign = s_empty;
    s_conventions.negative_sign = s_empty;
    s_conventions.int_frac_digits = s_conventions.frac_digits = s_conventions.p_cs_precedes = s_conventions.p_sep_by_space = CHAR_MAX;
    s_conventions.n_cs_precedes = s_conventions.n_sep_by_space = s_conventions.p_sign_posn = s_conventions.n_sign_posn = CHAR_MAX;
    s_conventions.int_p_cs_precedes = s_conventions.int_n_cs_precedes = s_conventions.int_p_sep_by_space = s_conventions.int_n_sep_by_space = CHAR_MAX;
    s_conventions.int_p_sign_posn = s_conventions.int_n_sign_posn = CHAR_MAX;
    if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
        return;
    auto const file = load(locale, "LC_MONETARY");
    if (!file.text)
        return;
    s_conventions.int_curr_symbol = const_cast<char*>(line_of(file, 0, ""));
    s_conventions.currency_symbol = const_cast<char*>(line_of(file, 1, ""));
    s_conventions.mon_decimal_point = const_cast<char*>(line_of(file, 2, ""));
    s_conventions.mon_thousands_sep = const_cast<char*>(line_of(file, 3, ""));
    s_conventions.mon_grouping = grouping_from(line_of(file, 4, ""));
    s_conventions.positive_sign = const_cast<char*>(line_of(file, 5, ""));
    s_conventions.negative_sign = const_cast<char*>(line_of(file, 6, ""));
    s_conventions.int_frac_digits = number_from(file, 7);
    s_conventions.frac_digits = number_from(file, 8);
    s_conventions.p_cs_precedes = number_from(file, 9);
    s_conventions.p_sep_by_space = number_from(file, 10);
    s_conventions.n_cs_precedes = number_from(file, 11);
    s_conventions.n_sep_by_space = number_from(file, 12);
    s_conventions.p_sign_posn = number_from(file, 13);
    s_conventions.n_sign_posn = number_from(file, 14);
    s_conventions.int_p_cs_precedes = s_conventions.p_cs_precedes;
    s_conventions.int_n_cs_precedes = s_conventions.n_cs_precedes;
    s_conventions.int_p_sep_by_space = s_conventions.p_sep_by_space;
    s_conventions.int_n_sep_by_space = s_conventions.n_sep_by_space;
    s_conventions.int_p_sign_posn = s_conventions.p_sign_posn;
    s_conventions.int_n_sign_posn = s_conventions.n_sign_posn;
}

// ---- Which it is.

bool exists(char const* name)
{
    if (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0 || strcmp(name, "UTF-8") == 0)
        return true;
    if (strlen(name) >= longest_name || strchr(name, '/') || strstr(name, ".."))
        return false;
    char path[128 + longest_name];
    snprintf(path, sizeof(path), "/usr/share/locale/%s", name);
    struct stat information {};
    return stat(path, &information) == 0 && S_ISDIR(information.st_mode);
}

// What the environment says this category is, for "".
char const* from_environment(int category)
{
    char const* value = getenv("LC_ALL");
    if (!value || !*value)
        value = getenv(s_category_names[category]);
    if (!value || !*value)
        value = getenv("LANG");
    return value && *value ? value : "C";
}

void apply(int category, char const* name)
{
    strcpy(s_names[category], name);
    switch (category) {
    case LC_TIME:
        set_time_from(name);
        break;
    case LC_NUMERIC:
        set_numbers_from(name);
        break;
    case LC_MONETARY:
        set_money_from(name);
        break;
    default:
        break;
    }
}

char* all_name()
{
    bool same = true;
    for (int category = 2; category < category_count; ++category)
        same = same && strcmp(s_names[category], s_names[1]) == 0;
    if (same)
        return s_names[1];
    s_all_name[0] = 0;
    for (int category = 1; category < category_count; ++category) {
        if (category > 1)
            strcat(s_all_name, "/");
        strcat(s_all_name, s_names[category]);
    }
    return s_all_name;
}

}

namespace LibC {

TimeNames const& time_names()
{
    return s_time;
}

}

extern "C" {

char* setlocale(int category, char const* locale)
{
    if (category < 0 || category >= category_count)
        return nullptr;
    LibC::LockGuard guard(s_lock);
    if (!locale)
        return category == LC_ALL ? all_name() : s_names[category];
    int const first = category == LC_ALL ? 1 : category;
    int const last = category == LC_ALL ? category_count - 1 : category;
    // All of it or none: nothing changes if any is wrong.
    for (int one = first; one <= last; ++one) {
        char const* name = *locale ? locale : from_environment(one);
        if (!exists(name))
            return nullptr;
    }
    for (int one = first; one <= last; ++one)
        apply(one, *locale ? locale : from_environment(one));
    return category == LC_ALL ? all_name() : s_names[category];
}

struct lconv* localeconv(void)
{
    return &s_conventions;
}
}
