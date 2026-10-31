/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Base {

constexpr bool is_ascii(u32 code_point) { return code_point < 0x80; }
constexpr bool is_ascii_digit(u32 code_point) { return code_point >= '0' && code_point <= '9'; }
constexpr bool is_ascii_upper_alpha(u32 code_point) { return code_point >= 'A' && code_point <= 'Z'; }
constexpr bool is_ascii_lower_alpha(u32 code_point) { return code_point >= 'a' && code_point <= 'z'; }
constexpr bool is_ascii_alpha(u32 code_point) { return is_ascii_upper_alpha(code_point) || is_ascii_lower_alpha(code_point); }
constexpr bool is_ascii_alphanumeric(u32 code_point) { return is_ascii_alpha(code_point) || is_ascii_digit(code_point); }
constexpr bool is_ascii_space(u32 code_point) { return code_point == ' ' || (code_point >= '\t' && code_point <= '\r'); }
constexpr bool is_ascii_printable(u32 code_point) { return code_point >= 0x20 && code_point <= 0x7e; }

constexpr bool is_ascii_hex_digit(u32 code_point)
{
    return is_ascii_digit(code_point) || (code_point >= 'a' && code_point <= 'f') || (code_point >= 'A' && code_point <= 'F');
}

// `code_point` must satisfy is_ascii_hex_digit().
constexpr u32 parse_ascii_hex_digit(u32 code_point)
{
    if (is_ascii_digit(code_point))
        return code_point - '0';
    if (code_point >= 'a' && code_point <= 'f')
        return code_point - 'a' + 10;
    return code_point - 'A' + 10;
}

constexpr u32 to_ascii_lowercase(u32 code_point) { return is_ascii_upper_alpha(code_point) ? code_point + 0x20 : code_point; }
constexpr u32 to_ascii_uppercase(u32 code_point) { return is_ascii_lower_alpha(code_point) ? code_point - 0x20 : code_point; }

}

using Base::is_ascii;
using Base::is_ascii_alpha;
using Base::is_ascii_alphanumeric;
using Base::is_ascii_digit;
using Base::is_ascii_hex_digit;
using Base::is_ascii_lower_alpha;
using Base::is_ascii_printable;
using Base::is_ascii_space;
using Base::is_ascii_upper_alpha;
using Base::parse_ascii_hex_digit;
using Base::to_ascii_lowercase;
using Base::to_ascii_uppercase;
