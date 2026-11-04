/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <stdio.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// wchar_t is a code point, 32 bits, as on a Mac. A multibyte string is always UTF-8.
typedef unsigned int wint_t;
#define WEOF ((wint_t)-1)
#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647

typedef struct {
    // A character that a buffer ended in the middle of: how much of it is here, and what it comes to so far.
    unsigned int value;
    int remaining;
} mbstate_t;

size_t wcslen(wchar_t const*);
wchar_t* wcscpy(wchar_t*, wchar_t const*);
wchar_t* wcsncpy(wchar_t*, wchar_t const*, size_t);
wchar_t* wcscat(wchar_t*, wchar_t const*);
wchar_t* wcsncat(wchar_t*, wchar_t const*, size_t);
int wcscmp(wchar_t const*, wchar_t const*);
int wcsncmp(wchar_t const*, wchar_t const*, size_t);
wchar_t* wcschr(wchar_t const*, wchar_t);
wchar_t* wcsrchr(wchar_t const*, wchar_t);
wchar_t* wcsstr(wchar_t const*, wchar_t const*);
size_t wcsspn(wchar_t const*, wchar_t const*);
size_t wcscspn(wchar_t const*, wchar_t const*);
wchar_t* wmemchr(wchar_t const*, wchar_t, size_t);
int wmemcmp(wchar_t const*, wchar_t const*, size_t);
wchar_t* wmemcpy(wchar_t*, wchar_t const*, size_t);
wchar_t* wmemmove(wchar_t*, wchar_t const*, size_t);
wchar_t* wmemset(wchar_t*, wchar_t, size_t);
long wcstol(wchar_t const*, wchar_t**, int);
unsigned long wcstoul(wchar_t const*, wchar_t**, int);
double wcstod(wchar_t const*, wchar_t**);

// How wide it is on a terminal: 0, 1 or 2 columns, -1 for what can't be shown.
int wcwidth(wchar_t);
int wcswidth(wchar_t const*, size_t);

size_t mbrtowc(wchar_t*, char const*, size_t, mbstate_t*);
size_t wcrtomb(char*, wchar_t, mbstate_t*);
size_t mbrlen(char const*, size_t, mbstate_t*);
size_t mbsrtowcs(wchar_t*, char const**, size_t, mbstate_t*);
size_t wcsrtombs(char*, wchar_t const**, size_t, mbstate_t*);
int mbsinit(mbstate_t const*);
wint_t btowc(int);
int wctob(wint_t);

int swprintf(wchar_t*, size_t, wchar_t const*, ...);
wint_t fgetwc(FILE*);
wint_t fputwc(wchar_t, FILE*);
wint_t getwc(FILE*);
wint_t putwc(wchar_t, FILE*);
wint_t getwchar(void);
wint_t putwchar(wchar_t);
int fputws(wchar_t const*, FILE*);

__END_DECLS
