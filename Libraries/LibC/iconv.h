/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

typedef void* iconv_t;

// Unicode's own forms, ASCII, Latin-1 and the other ISO-8859 sets, the Windows and Mac code pages, KOI8, Shift_JIS, EUC-JP, EUC-KR, GBK, GB18030 and Big5.
iconv_t iconv_open(char const* to, char const* from);
size_t iconv(iconv_t, char** in, size_t* in_left, char** out, size_t* out_left);
int iconv_close(iconv_t);

__END_DECLS
