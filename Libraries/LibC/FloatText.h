/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>

// Numbers to text and back, exactly: what's printed is the true value of the number, correctly rounded, and what's read is the nearest number to what
// the text says. It takes whole-number arithmetic with a lot of digits to be sure of that, which is why it's all in here.

namespace LibC {

// As printf's conversion (f, F, e, E, g, G, a, A) has it, of a finite number that isn't negative; the sign's the caller's. A negative precision means
// the default. Returns how long it is. There's room for as many digits as a number can have, and a precision of up to `size` less than that.
size_t format_float(char* buffer, size_t size, double value, char conversion, int precision, bool alternate);

// The number at the start of `text`, which has no white space or sign: decimal, or hexadecimal after a "0x". `mantissa_bits` is 53 for a double, and 24
// for a float. Sets `end`, and whether it was too large or too small to be one.
double parse_float(char const* text, char const** end, int mantissa_bits, bool& out_of_range);

}
