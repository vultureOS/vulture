/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <wchar.h>

__BEGIN_DECLS

typedef unsigned long wctype_t;

// For ASCII, and Latin letters' case. M17's Unicode tables bring the rest.
int iswalnum(wint_t);
int iswalpha(wint_t);
int iswblank(wint_t);
int iswcntrl(wint_t);
int iswdigit(wint_t);
int iswgraph(wint_t);
int iswlower(wint_t);
int iswprint(wint_t);
int iswpunct(wint_t);
int iswspace(wint_t);
int iswupper(wint_t);
int iswxdigit(wint_t);
wint_t towlower(wint_t);
wint_t towupper(wint_t);
wctype_t wctype(char const*);
int iswctype(wint_t, wctype_t);

__END_DECLS
