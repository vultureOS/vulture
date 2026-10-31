/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace {

constexpr char const* s_weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
constexpr char const* s_months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

// Days since 1970-01-01 for a civil date, and back (Howard Hinnant's
// algorithms, valid far beyond time_t's range).
long long days_from_civil(long long year, unsigned month, unsigned day)
{
    year -= month <= 2;
    long long era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<long long>(day_of_era) - 719468;
}

void civil_from_days(long long days, long long& year, unsigned& month, unsigned& day)
{
    days += 719468;
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    year = static_cast<long long>(year_of_era) + era * 400;
    unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    unsigned month_index = (5 * day_of_year + 2) / 153;
    day = day_of_year - (153 * month_index + 2) / 5 + 1;
    month = month_index < 10 ? month_index + 3 : month_index - 9;
    year += month <= 2;
}

void fill_tm(time_t time, long offset, char const* zone, struct tm* result)
{
    long long local = time + offset;
    long long days = local / 86400;
    long long seconds = local % 86400;
    if (seconds < 0) {
        seconds += 86400;
        --days;
    }
    long long year;
    unsigned month, day;
    civil_from_days(days, year, month, day);
    result->tm_year = static_cast<int>(year - 1900);
    result->tm_mon = static_cast<int>(month - 1);
    result->tm_mday = static_cast<int>(day);
    result->tm_hour = static_cast<int>(seconds / 3600);
    result->tm_min = static_cast<int>(seconds / 60 % 60);
    result->tm_sec = static_cast<int>(seconds % 60);
    result->tm_wday = static_cast<int>(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday.
    result->tm_yday = static_cast<int>(days - days_from_civil(year, 1, 1));
    result->tm_isdst = 0;
    result->tm_gmtoff = offset;
    result->tm_zone = zone;
}

long s_offset = 0; // Seconds east of UTC.
char s_zone_name[16] = "UTC";

}

extern "C" {

long timezone = 0;
int daylight = 0;
char* tzname[2] = { s_zone_name, s_zone_name };

// TZ as a fixed offset: "UTC", "GMT", "UTC+5", "UTC-3:30", or POSIX
// "EST5" (west of UTC is positive there).
void tzset(void)
{
    char const* tz = getenv("TZ");
    s_offset = 0;
    strcpy(s_zone_name, "UTC");
    if (tz && *tz) {
        char const* cursor = tz;
        if (*cursor == ':')
            ++cursor;
        size_t name_length = 0;
        while (isalpha(static_cast<unsigned char>(cursor[name_length])))
            ++name_length;
        bool posix_style = name_length > 0 && strncmp(cursor, "UTC", 3) != 0 && strncmp(cursor, "GMT", 3) != 0;
        if (name_length > 0 && name_length < sizeof(s_zone_name)) {
            memcpy(s_zone_name, cursor, name_length);
            s_zone_name[name_length] = '\0';
        }
        cursor += name_length;
        if (*cursor == '+' || *cursor == '-' || isdigit(static_cast<unsigned char>(*cursor))) {
            int sign = *cursor == '-' ? -1 : 1;
            if (*cursor == '+' || *cursor == '-')
                ++cursor;
            long hours = strtol(cursor, const_cast<char**>(&cursor), 10);
            long minutes = 0;
            if (*cursor == ':')
                minutes = strtol(cursor + 1, nullptr, 10);
            long offset = sign * (hours * 3600 + minutes * 60);
            // POSIX TZ counts hours west of UTC.
            s_offset = posix_style ? -offset : offset;
        }
    }
    timezone = -s_offset;
}

struct tm* gmtime_r(time_t const* time, struct tm* result)
{
    fill_tm(*time, 0, "UTC", result);
    return result;
}

struct tm* gmtime(time_t const* time)
{
    static struct tm result;
    return gmtime_r(time, &result);
}

struct tm* localtime_r(time_t const* time, struct tm* result)
{
    tzset();
    fill_tm(*time, s_offset, s_zone_name, result);
    return result;
}

struct tm* localtime(time_t const* time)
{
    static struct tm result;
    return localtime_r(time, &result);
}

time_t timegm(struct tm* broken)
{
    // Normalize months (and through days, everything else).
    long long year = broken->tm_year + 1900LL + broken->tm_mon / 12;
    int month = broken->tm_mon % 12;
    if (month < 0) {
        month += 12;
        --year;
    }
    long long days = days_from_civil(year, static_cast<unsigned>(month + 1), 1) + broken->tm_mday - 1;
    long long seconds = days * 86400 + broken->tm_hour * 3600LL + broken->tm_min * 60LL + broken->tm_sec;
    fill_tm(seconds, 0, "UTC", broken);
    return seconds;
}

time_t mktime(struct tm* broken)
{
    tzset();
    struct tm copy = *broken;
    time_t utc = timegm(&copy);
    time_t result = utc - s_offset;
    fill_tm(result, s_offset, s_zone_name, broken);
    return result;
}

double difftime(time_t end, time_t start)
{
    return static_cast<double>(end - start);
}

clock_t clock(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<clock_t>(now.tv_sec * CLOCKS_PER_SEC + now.tv_nsec / 1000);
}

int clock_getres(clockid_t, struct timespec* resolution)
{
    if (resolution) {
        resolution->tv_sec = 0;
        resolution->tv_nsec = 42; // The 24 MHz counter.
    }
    return 0;
}

int gettimeofday(struct timeval* result, void*)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    result->tv_sec = now.tv_sec;
    result->tv_usec = now.tv_nsec / 1000;
    return 0;
}

size_t strftime(char* buffer, size_t size, char const* format, struct tm const* broken)
{
    size_t length = 0;
    auto append = [&](char const* text) {
        while (*text) {
            if (length + 1 < size)
                buffer[length] = *text;
            ++length;
            ++text;
        }
    };
    auto number = [&](long value, int width, char pad) {
        char text[24];
        snprintf(text, sizeof(text), pad == '0' ? "%0*ld" : "%*ld", width, value);
        append(text);
    };
    for (char const* cursor = format; *cursor; ++cursor) {
        if (*cursor != '%') {
            char text[2] = { *cursor, '\0' };
            append(text);
            continue;
        }
        ++cursor;
        // Flags: '-' (no padding), '_' (spaces), and E/O modifiers ignored.
        char pad = '0';
        if (*cursor == '-' || *cursor == '_') {
            pad = *cursor == '-' ? '\0' : ' ';
            ++cursor;
        }
        if (*cursor == 'E' || *cursor == 'O')
            ++cursor;
        int width2 = pad == '\0' ? 1 : 2;
        char p = pad == '\0' ? '0' : pad;
        switch (*cursor) {
        case 'a':
            append(broken->tm_wday >= 0 && broken->tm_wday < 7 ? s_weekdays[broken->tm_wday] : "?");
            length -= (broken->tm_wday >= 0 && broken->tm_wday < 7) ? strlen(s_weekdays[broken->tm_wday]) - 3 : 0;
            break;
        case 'A':
            append(broken->tm_wday >= 0 && broken->tm_wday < 7 ? s_weekdays[broken->tm_wday] : "?");
            break;
        case 'b':
        case 'h': {
            char text[4] = { 0 };
            if (broken->tm_mon >= 0 && broken->tm_mon < 12)
                memcpy(text, s_months[broken->tm_mon], 3);
            append(text);
            break;
        }
        case 'B':
            append(broken->tm_mon >= 0 && broken->tm_mon < 12 ? s_months[broken->tm_mon] : "?");
            break;
        case 'c': {
            char text[64];
            strftime(text, sizeof(text), "%a %b %e %H:%M:%S %Y", broken);
            append(text);
            break;
        }
        case 'C':
            number((broken->tm_year + 1900) / 100, width2, p);
            break;
        case 'd':
            number(broken->tm_mday, width2, p);
            break;
        case 'D': {
            char text[16];
            strftime(text, sizeof(text), "%m/%d/%y", broken);
            append(text);
            break;
        }
        case 'e':
            number(broken->tm_mday, 2, pad == '0' ? ' ' : p);
            break;
        case 'F': {
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%d", broken);
            append(text);
            break;
        }
        case 'H':
            number(broken->tm_hour, width2, p);
            break;
        case 'I':
            number(broken->tm_hour % 12 ? broken->tm_hour % 12 : 12, width2, p);
            break;
        case 'j':
            number(broken->tm_yday + 1, pad == '\0' ? 1 : 3, p);
            break;
        case 'k':
            number(broken->tm_hour, 2, ' ');
            break;
        case 'l':
            number(broken->tm_hour % 12 ? broken->tm_hour % 12 : 12, 2, ' ');
            break;
        case 'm':
            number(broken->tm_mon + 1, width2, p);
            break;
        case 'M':
            number(broken->tm_min, width2, p);
            break;
        case 'n':
            append("\n");
            break;
        case 'p':
            append(broken->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'r': {
            char text[32];
            strftime(text, sizeof(text), "%I:%M:%S %p", broken);
            append(text);
            break;
        }
        case 'R': {
            char text[16];
            strftime(text, sizeof(text), "%H:%M", broken);
            append(text);
            break;
        }
        case 's': {
            struct tm copy = *broken;
            number(static_cast<long>(mktime(&copy)), 1, '0');
            break;
        }
        case 'S':
            number(broken->tm_sec, width2, p);
            break;
        case 't':
            append("\t");
            break;
        case 'T': {
            char text[16];
            strftime(text, sizeof(text), "%H:%M:%S", broken);
            append(text);
            break;
        }
        case 'u':
            number(broken->tm_wday == 0 ? 7 : broken->tm_wday, 1, '0');
            break;
        case 'w':
            number(broken->tm_wday, 1, '0');
            break;
        case 'x': {
            char text[16];
            strftime(text, sizeof(text), "%m/%d/%y", broken);
            append(text);
            break;
        }
        case 'X': {
            char text[16];
            strftime(text, sizeof(text), "%H:%M:%S", broken);
            append(text);
            break;
        }
        case 'y':
            number((broken->tm_year + 1900) % 100, width2, p);
            break;
        case 'Y':
            number(broken->tm_year + 1900L, 1, '0');
            break;
        case 'z': {
            long offset = broken->tm_gmtoff;
            char text[8];
            snprintf(text, sizeof(text), "%c%02ld%02ld", offset < 0 ? '-' : '+', (offset < 0 ? -offset : offset) / 3600, (offset < 0 ? -offset : offset) / 60 % 60);
            append(text);
            break;
        }
        case 'Z':
            append(broken->tm_zone ? broken->tm_zone : "UTC");
            break;
        case '%':
            append("%");
            break;
        default:
            break;
        }
    }
    if (size > 0)
        buffer[length < size ? length : size - 1] = '\0';
    return length < size ? length : 0;
}

char* asctime_r(struct tm const* broken, char* buffer)
{
    strftime(buffer, 26, "%a %b %e %H:%M:%S %Y\n", broken);
    return buffer;
}

char* asctime(struct tm const* broken)
{
    static char buffer[32];
    return asctime_r(broken, buffer);
}

char* ctime_r(time_t const* time, char* buffer)
{
    struct tm broken;
    return asctime_r(localtime_r(time, &broken), buffer);
}

char* ctime(time_t const* time)
{
    static char buffer[32];
    return ctime_r(time, buffer);
}
}
