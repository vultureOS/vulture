/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// What iconv knows besides Unicode's own: the older ways of writing text, by the Encoding Standard's tables (Generated/EncodingTables.cpp). Those for CJK are large, but they're
// only paged in if they're used.

namespace LibC::Encodings {

constexpr size_t single_byte_count = 27;

extern char const* const single_byte_names[single_byte_count];
extern u16 const single_byte_tables[single_byte_count][128];

extern u16 const jis0208[];
extern size_t const jis0208_count;
extern u16 const jis0212[];
extern size_t const jis0212_count;
extern u16 const euc_kr[];
extern size_t const euc_kr_count;
extern u16 const gb18030[];
extern size_t const gb18030_count;
extern u32 const big5[];
extern size_t const big5_count;
extern u32 const gb18030_ranges[];
extern size_t const gb18030_ranges_count;

enum class MultiByte : u8 {
    ShiftJIS,
    EUCJP,
    EUCKR,
    GBK,
    GB18030,
    Big5,
};

// How many bytes it took; 0 if there's not enough to tell; -1 if it's wrong. As iconv's own decode.
int decode(MultiByte, unsigned char const* in, size_t left, unsigned& code);
// How many bytes it came to; 0 if it won't fit; -1 if this can't say it.
int encode(MultiByte, unsigned code, unsigned char* out, size_t left);

}
