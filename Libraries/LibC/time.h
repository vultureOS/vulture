/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/time.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef int clockid_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

int clock_gettime(clockid_t, struct timespec*);
int clock_getres(clockid_t, struct timespec*);
int nanosleep(struct timespec const* duration, struct timespec* remaining);
time_t time(time_t* result);

typedef long clock_t;
#define CLOCKS_PER_SEC 1000000

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;   // 0-11
    int tm_year;  // Years since 1900.
    int tm_wday;  // 0 = Sunday
    int tm_yday;  // 0-365
    int tm_isdst;
    long tm_gmtoff;
    char const* tm_zone;
};

// Local time follows TZ when it is a fixed offset ("UTC", "UTC+5:30",
// "PST8"), UTC otherwise; there is no time zone database yet.
extern long timezone;
extern int daylight;
extern char* tzname[2];
void tzset(void);

struct tm* gmtime(time_t const*);
struct tm* gmtime_r(time_t const*, struct tm*);
struct tm* localtime(time_t const*);
struct tm* localtime_r(time_t const*, struct tm*);
time_t mktime(struct tm*);
time_t timegm(struct tm*);
size_t strftime(char* buffer, size_t size, char const* format, struct tm const*);
// The reverse of strftime for the common conversions (%a %A %b %B %h %c
// %C %d %e %D %H %I %j %m %M %n %t %p %r %R %S %T %y %Y %z %Z %%); where
// parsing stopped, or nothing if the text does not match.
char* strptime(char const* text, char const* format, struct tm*);
char* asctime(struct tm const*);
char* asctime_r(struct tm const*, char* buffer);
char* ctime(time_t const*);
char* ctime_r(time_t const*, char* buffer);
double difftime(time_t end, time_t start);
clock_t clock(void);

__END_DECLS
