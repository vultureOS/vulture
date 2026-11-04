/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FloatText.h"
#include <LibBase/Types.h>
#include <string.h>

namespace LibC {

namespace {

// ---- A number's digits.

// The most a double has, in its integer part (DBL_MAX) and in its fraction (the smallest subnormal).
constexpr size_t most_digits = 309 + 1074 + 8;

// The exact value, as 0.d1d2d3... x 10^point, with no zeros at either end of the digits.
struct Decimal {
    char digits[most_digits];
    int count { 0 };
    int point { 0 };
};

void take_apart(double value, u64& mantissa, int& exponent)
{
    u64 bits;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    int const biased = static_cast<int>((bits >> 52) & 0x7ff);
    mantissa = bits & ((1ull << 52) - 1);
    if (biased == 0) {
        exponent = -1074;
    } else {
        mantissa |= 1ull << 52;
        exponent = biased - 1075;
    }
}

void to_decimal(double value, Decimal& out)
{
    u64 mantissa;
    int exponent;
    take_apart(value, mantissa, exponent);
    out.count = 0;
    out.point = 0;
    if (mantissa == 0)
        return;

    // The whole part: as words, from the lowest, then nine digits at a time from the lowest.
    u32 words[36] {};
    size_t word_count = 0;
    u64 fraction_bits = 0;
    int fraction_length = 0;
    if (exponent >= 0) {
        size_t const whole_words = static_cast<size_t>(exponent) / 32;
        int const shift = exponent % 32;
        // (shift < 32, so this all fits in the next two words and a bit.)
        u64 const low = mantissa << shift;
        u64 const high = shift ? mantissa >> (64 - shift) : 0;
        words[whole_words] = static_cast<u32>(low);
        words[whole_words + 1] = static_cast<u32>(low >> 32);
        words[whole_words + 2] = static_cast<u32>(high);
        word_count = whole_words + 3;
    } else {
        fraction_length = -exponent;
        if (fraction_length < 64) {
            u64 const whole = mantissa >> fraction_length;
            fraction_bits = mantissa & ((1ull << fraction_length) - 1);
            words[0] = static_cast<u32>(whole);
            words[1] = static_cast<u32>(whole >> 32);
            word_count = 2;
        } else {
            fraction_bits = mantissa;
        }
    }

    char whole_digits[320];
    int whole_count = 0; // From the lowest.
    for (;;) {
        while (word_count > 0 && words[word_count - 1] == 0)
            --word_count;
        if (word_count == 0)
            break;
        u64 remainder = 0;
        for (size_t i = word_count; i > 0; --i) {
            u64 const current = (remainder << 32) | words[i - 1];
            words[i - 1] = static_cast<u32>(current / 1000000000u);
            remainder = current % 1000000000u;
        }
        for (int i = 0; i < 9; ++i) {
            whole_digits[whole_count++] = static_cast<char>('0' + remainder % 10);
            remainder /= 10;
        }
    }
    while (whole_count > 0 && whole_digits[whole_count - 1] == '0')
        --whole_count;
    for (int i = whole_count; i > 0; --i)
        out.digits[out.count++] = whole_digits[i - 1];
    out.point = whole_count;

    // The fraction: multiplied by a billion, what comes out at the top is the next nine digits.
    if (fraction_length > 0 && fraction_bits != 0) {
        // In whole words, so that what overflows is what comes out.
        size_t const fraction_words = (static_cast<size_t>(fraction_length) + 31) / 32;
        int const left = static_cast<int>(fraction_words * 32) - fraction_length;
        u32 fraction[36] {};
        // Where the mantissa's lowest bit goes, counting from the bottom of it all.
        u64 const low = fraction_bits << left;
        u64 const high = left ? fraction_bits >> (64 - left) : 0;
        fraction[0] = static_cast<u32>(low);
        if (fraction_words > 1)
            fraction[1] = static_cast<u32>(low >> 32);
        if (fraction_words > 2)
            fraction[2] = static_cast<u32>(high);
        bool started = whole_count > 0;
        for (;;) {
            bool any = false;
            for (size_t i = 0; i < fraction_words; ++i)
                any = any || fraction[i] != 0;
            if (!any)
                break;
            u64 carry = 0;
            for (size_t i = 0; i < fraction_words; ++i) {
                u64 const current = static_cast<u64>(fraction[i]) * 1000000000u + carry;
                fraction[i] = static_cast<u32>(current);
                carry = current >> 32;
            }
            char nine[9];
            for (int i = 8; i >= 0; --i) {
                nine[i] = static_cast<char>('0' + carry % 10);
                carry /= 10;
            }
            for (int i = 0; i < 9; ++i) {
                if (!started && nine[i] == '0') {
                    --out.point;
                    continue;
                }
                started = true;
                out.digits[out.count++] = nine[i];
            }
        }
    }
    while (out.count > 0 && out.digits[out.count - 1] == '0')
        --out.count;
}

// To `keep` digits, the nearest, and the even one if it's exactly half way.
void round_to(Decimal& decimal, int keep)
{
    if (keep >= decimal.count)
        return;
    if (keep < 0) {
        decimal.count = 0;
        return;
    }
    char const next = decimal.digits[keep];
    bool round_up;
    if (next != '5') {
        round_up = next > '5';
    } else {
        bool beyond = false;
        for (int i = keep + 1; i < decimal.count; ++i)
            beyond = beyond || decimal.digits[i] != '0';
        round_up = beyond || (keep > 0 && ((decimal.digits[keep - 1] - '0') & 1));
    }
    decimal.count = keep;
    if (round_up) {
        int i = keep - 1;
        for (; i >= 0; --i) {
            if (decimal.digits[i] != '9') {
                ++decimal.digits[i];
                break;
            }
            decimal.digits[i] = '0';
        }
        if (i < 0) {
            // All nines, or nothing kept: the next power of ten.
            decimal.digits[0] = '1';
            decimal.count = 1;
            ++decimal.point;
        }
    }
    while (decimal.count > 0 && decimal.digits[decimal.count - 1] == '0')
        --decimal.count;
}

struct Writer {
    char* buffer;
    size_t size;
    size_t length { 0 };
    void put(char c)
    {
        if (length < size)
            buffer[length] = c;
        ++length;
    }
};

void write_fixed(Writer& out, Decimal const& decimal, int precision, bool alternate)
{
    if (decimal.point <= 0) {
        out.put('0');
    } else {
        for (int i = 0; i < decimal.point; ++i)
            out.put(i < decimal.count ? decimal.digits[i] : '0');
    }
    if (precision > 0 || alternate)
        out.put('.');
    for (int i = 0; i < precision; ++i) {
        int const index = decimal.point + i;
        out.put(index >= 0 && index < decimal.count ? decimal.digits[index] : '0');
    }
}

void write_exponent(Writer& out, int exponent, char letter)
{
    out.put(letter);
    out.put(exponent < 0 ? '-' : '+');
    int const magnitude = exponent < 0 ? -exponent : exponent;
    if (magnitude >= 100)
        out.put(static_cast<char>('0' + magnitude / 100));
    out.put(static_cast<char>('0' + (magnitude / 10) % 10));
    out.put(static_cast<char>('0' + magnitude % 10));
}

void write_scientific(Writer& out, Decimal const& decimal, int precision, bool alternate, char letter)
{
    out.put(decimal.count ? decimal.digits[0] : '0');
    if (precision > 0 || alternate)
        out.put('.');
    for (int i = 1; i <= precision; ++i)
        out.put(i < decimal.count ? decimal.digits[i] : '0');
    write_exponent(out, decimal.count ? decimal.point - 1 : 0, letter);
}

void write_hexadecimal(Writer& out, double value, int precision, bool alternate, bool upper)
{
    char const* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    u64 mantissa;
    int exponent;
    take_apart(value, mantissa, exponent);
    out.put('0');
    out.put(upper ? 'X' : 'x');
    if (mantissa == 0) {
        out.put('0');
        if (precision > 0 || alternate)
            out.put('.');
        for (int i = 0; i < precision; ++i)
            out.put('0');
        out.put(upper ? 'P' : 'p');
        out.put('+');
        out.put('0');
        return;
    }
    // With its first bit on its own, even if that takes moving it: a smallest number is 0x1p-1074.
    while (!(mantissa & (1ull << 52))) {
        mantissa <<= 1;
        --exponent;
    }
    exponent += 52;
    u64 fraction = mantissa & ((1ull << 52) - 1);
    u64 leading = 1;
    if (precision >= 0 && precision < 13) {
        int const dropped = (13 - precision) * 4;
        u64 const half = 1ull << (dropped - 1);
        u64 const rest = fraction & ((1ull << dropped) - 1);
        fraction >>= dropped;
        bool const odd = precision > 0 ? (fraction & 1) : (leading & 1);
        if (rest > half || (rest == half && odd)) {
            ++fraction;
            if (precision == 0 || (fraction >> (precision * 4))) {
                ++leading;
                fraction = 0;
            }
        }
        fraction <<= dropped;
    }
    out.put(static_cast<char>('0' + leading));
    int nibbles = 13;
    if (precision < 0) {
        while (nibbles > 0 && ((fraction >> ((13 - nibbles) * 4)) & 0xf) == 0)
            --nibbles;
    } else {
        nibbles = precision;
    }
    if (nibbles > 0 || alternate)
        out.put('.');
    for (int i = 0; i < nibbles; ++i)
        out.put(i < 13 ? alphabet[(fraction >> ((12 - i) * 4)) & 0xf] : '0');
    out.put(upper ? 'P' : 'p');
    out.put(exponent < 0 ? '-' : '+');
    int const magnitude = exponent < 0 ? -exponent : exponent;
    char digits[8];
    int count = 0;
    int remaining = magnitude;
    do {
        digits[count++] = static_cast<char>('0' + remaining % 10);
        remaining /= 10;
    } while (remaining);
    while (count)
        out.put(digits[--count]);
}

}

size_t format_float(char* buffer, size_t size, double value, char conversion, int precision, bool alternate)
{
    Writer out { buffer, size };
    bool const upper = conversion >= 'A' && conversion <= 'Z';
    char const lower = upper ? static_cast<char>(conversion - 'A' + 'a') : conversion;
    if (lower == 'a') {
        write_hexadecimal(out, value, precision, alternate, upper);
        return out.length < size ? out.length : size;
    }
    static_assert(sizeof(Decimal) < 2048);
    Decimal decimal;
    to_decimal(value, decimal);
    int const wanted = precision < 0 ? 6 : precision;
    switch (lower) {
    case 'f':
        round_to(decimal, decimal.point + wanted);
        write_fixed(out, decimal, wanted, alternate);
        break;
    case 'e':
        round_to(decimal, wanted + 1);
        write_scientific(out, decimal, wanted, alternate, upper ? 'E' : 'e');
        break;
    default: {
        // As few as make sense, with the point where it's easiest to read.
        int const significant = wanted == 0 ? 1 : wanted;
        round_to(decimal, significant);
        int const exponent = decimal.count ? decimal.point - 1 : 0;
        size_t const start = out.length;
        bool const fixed = exponent >= -4 && exponent < significant;
        int const places = fixed ? significant - 1 - exponent : significant - 1;
        if (fixed)
            write_fixed(out, decimal, places, alternate);
        else
            write_scientific(out, decimal, places, alternate, upper ? 'E' : 'e');
        if (!alternate) {
            // Without zeros at the end of the fraction, which aren't telling anybody anything.
            size_t mantissa_end = out.length;
            if (!fixed) {
                mantissa_end = start;
                while (mantissa_end < out.length && out.buffer[mantissa_end] != 'e' && out.buffer[mantissa_end] != 'E')
                    ++mantissa_end;
            }
            bool has_point = false;
            for (size_t i = start; i < mantissa_end; ++i)
                has_point = has_point || out.buffer[i] == '.';
            if (has_point) {
                size_t trimmed = mantissa_end;
                while (trimmed > start && out.buffer[trimmed - 1] == '0')
                    --trimmed;
                if (trimmed > start && out.buffer[trimmed - 1] == '.')
                    --trimmed;
                size_t const tail = out.length - mantissa_end;
                memmove(out.buffer + trimmed, out.buffer + mantissa_end, tail);
                out.length = trimmed + tail;
            }
        }
        break;
    }
    }
    return out.length < size ? out.length : size;
}

// ---- Reading them.

namespace {

// As large as it takes for the longest number that makes a difference.
constexpr size_t big_words = 256;

struct BigNumber {
    u32 words[big_words];
    size_t count { 0 };

    void set(u64 value)
    {
        words[0] = static_cast<u32>(value);
        words[1] = static_cast<u32>(value >> 32);
        count = words[1] ? 2 : (words[0] ? 1 : 0);
    }

    void multiply_add(u32 factor, u32 addend)
    {
        u64 carry = addend;
        for (size_t i = 0; i < count; ++i) {
            u64 const current = static_cast<u64>(words[i]) * factor + carry;
            words[i] = static_cast<u32>(current);
            carry = current >> 32;
        }
        if (carry && count < big_words)
            words[count++] = static_cast<u32>(carry);
    }

    [[nodiscard]] int bit_length() const
    {
        if (count == 0)
            return 0;
        return static_cast<int>((count - 1) * 32) + (32 - __builtin_clz(words[count - 1]));
    }

    void shift_left(int bits)
    {
        if (count == 0 || bits <= 0)
            return;
        size_t const whole = static_cast<size_t>(bits) / 32;
        int const part = bits % 32;
        size_t const new_count = count + whole + (part ? 1 : 0);
        size_t const limited = new_count > big_words ? big_words : new_count;
        for (size_t i = limited; i > 0; --i) {
            size_t const destination = i - 1;
            u32 value = 0;
            if (destination >= whole) {
                size_t const source = destination - whole;
                if (source < count)
                    value = words[source] << part;
                if (part && source > 0 && source - 1 < count)
                    value |= words[source - 1] >> (32 - part);
            }
            words[destination] = value;
        }
        count = limited;
        while (count > 0 && words[count - 1] == 0)
            --count;
    }

    [[nodiscard]] int compare(BigNumber const& other) const
    {
        if (count != other.count)
            return count < other.count ? -1 : 1;
        for (size_t i = count; i > 0; --i) {
            if (words[i - 1] != other.words[i - 1])
                return words[i - 1] < other.words[i - 1] ? -1 : 1;
        }
        return 0;
    }

    void subtract(BigNumber const& other)
    {
        i64 borrow = 0;
        for (size_t i = 0; i < count; ++i) {
            i64 const current = static_cast<i64>(words[i]) - (i < other.count ? other.words[i] : 0) + borrow;
            words[i] = static_cast<u32>(current);
            borrow = current >> 32;
        }
        while (count > 0 && words[count - 1] == 0)
            --count;
    }
};

// `whole` x 2^`exponent` (and a little more, if `more`), to the nearest there is.
double make_number(u64 whole, int exponent, bool more, int mantissa_bits, bool& out_of_range)
{
    if (whole == 0)
        return 0;
    int const length = 64 - __builtin_clzll(whole);
    int const top = exponent + length - 1; // The power of two of its first bit.
    int const max_exponent = mantissa_bits == 53 ? 1023 : 127;
    int const min_exponent = mantissa_bits == 53 ? -1022 : -126;
    int kept = mantissa_bits;
    if (top < min_exponent)
        kept = mantissa_bits - (min_exponent - top);
    int const dropped = length - kept;
    u64 result = whole;
    int result_exponent = exponent;
    if (kept <= 0) {
        // Below the smallest there is, or half of it at most.
        bool const round_up = kept == 0 && (whole > (1ull << (length - 1)) || more);
        if (!round_up) {
            out_of_range = true;
            return 0;
        }
        result = 1;
        result_exponent = exponent + length;
    } else if (dropped > 0) {
        u64 const lost = whole & ((dropped >= 64 ? 0 : (1ull << dropped)) - 1);
        u64 const half = 1ull << (dropped - 1);
        result = whole >> dropped;
        result_exponent = exponent + dropped;
        if (lost > half || (lost == half && (more || (result & 1))))
            ++result;
    } else if (dropped < 0) {
        result = whole << -dropped;
        result_exponent = exponent + dropped;
    }
    // Back together: as a double, whichever it's for. (A float's value is exactly a double's.)
    int const result_length = 64 - __builtin_clzll(result);
    if (result_exponent + result_length - 1 > max_exponent) {
        out_of_range = true;
        return __builtin_inf();
    }
    double value = static_cast<double>(result);
    // By steps, as a double's exponent might not reach in one.
    int remaining = result_exponent;
    while (remaining > 0) {
        int const step = remaining > 1000 ? 1000 : remaining;
        u64 const bits = static_cast<u64>(step + 1023) << 52;
        double factor;
        __builtin_memcpy(&factor, &bits, sizeof(factor));
        value *= factor;
        remaining -= step;
    }
    while (remaining < 0) {
        // (Exact: `result` has few enough bits by now that this is what it says, down to the smallest there is.)
        int const step = remaining < -1000 ? 1000 : -remaining;
        u64 const bits = static_cast<u64>(1023 - step) << 52;
        double factor;
        __builtin_memcpy(&factor, &bits, sizeof(factor));
        value *= factor;
        remaining += step;
    }
    return value;
}

double parse_hexadecimal(char const* text, char const** end, int mantissa_bits, bool& out_of_range)
{
    auto digit_value = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    char const* cursor = text;
    u64 whole = 0;
    int exponent = 0;
    bool more = false, any = false, past_point = false;
    for (;; ++cursor) {
        if (*cursor == '.' && !past_point) {
            past_point = true;
            continue;
        }
        int const digit = digit_value(*cursor);
        if (digit < 0)
            break;
        any = true;
        if (whole >> 60) {
            more = more || digit != 0;
            if (!past_point)
                exponent += 4;
        } else {
            whole = whole * 16 + static_cast<u64>(digit);
            if (past_point)
                exponent -= 4;
        }
    }
    if (!any) {
        *end = text; // Only "0x": the zero was the number.
        return 0;
    }
    if (*cursor == 'p' || *cursor == 'P') {
        char const* after = cursor + 1;
        bool negative = false;
        if (*after == '+' || *after == '-')
            negative = *after++ == '-';
        if (*after >= '0' && *after <= '9') {
            int value = 0;
            while (*after >= '0' && *after <= '9') {
                if (value < 100000)
                    value = value * 10 + (*after - '0');
                ++after;
            }
            exponent += negative ? -value : value;
            cursor = after;
        }
    }
    *end = cursor;
    return make_number(whole, exponent, more, mantissa_bits, out_of_range);
}

}

double parse_float(char const* text, char const** end, int mantissa_bits, bool& out_of_range)
{
    out_of_range = false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char const* after_hex = nullptr;
        double const value = parse_hexadecimal(text + 2, &after_hex, mantissa_bits, out_of_range);
        // Nothing after the "0x": the "0" was a number.
        *end = after_hex == text + 2 ? text + 1 : after_hex;
        return value;
    }

    // The digits that make a difference, without zeros at either end, and where the point is relative to the last of them.
    constexpr int most_significant = 800;
    char digits[most_significant];
    int digit_count = 0;
    long long exponent = 0;
    bool more = false, any = false, past_point = false;
    char const* cursor = text;
    for (;; ++cursor) {
        if (*cursor == '.' && !past_point) {
            past_point = true;
            continue;
        }
        if (*cursor < '0' || *cursor > '9')
            break;
        any = true;
        char const digit = *cursor;
        if (digit_count == 0 && digit == '0') {
            if (past_point)
                --exponent;
            continue;
        }
        if (digit_count < most_significant) {
            digits[digit_count++] = digit;
            if (past_point)
                --exponent;
        } else {
            more = more || digit != '0';
            if (!past_point)
                ++exponent;
        }
    }
    if (!any) {
        *end = text;
        return 0;
    }
    if (*cursor == 'e' || *cursor == 'E') {
        char const* after = cursor + 1;
        bool negative = false;
        if (*after == '+' || *after == '-')
            negative = *after++ == '-';
        if (*after >= '0' && *after <= '9') {
            long long value = 0;
            while (*after >= '0' && *after <= '9') {
                if (value < 1000000)
                    value = value * 10 + (*after - '0');
                ++after;
            }
            exponent += negative ? -value : value;
            cursor = after;
        }
    }
    *end = cursor;
    while (digit_count > 0 && digits[digit_count - 1] == '0') {
        --digit_count;
        ++exponent;
    }
    if (digit_count == 0)
        return 0;
    // So far off that it isn't worth working it out.
    if (exponent + digit_count > 400) {
        out_of_range = true;
        return __builtin_inf();
    }
    if (exponent + digit_count < -400) {
        out_of_range = true;
        return 0;
    }

    static_assert(sizeof(BigNumber) < 2048);
    BigNumber numerator, denominator;
    numerator.set(0);
    for (int i = 0; i < digit_count;) {
        u32 chunk = 0, scale = 1;
        for (int taken = 0; taken < 9 && i < digit_count; ++taken, ++i) {
            chunk = chunk * 10 + static_cast<u32>(digits[i] - '0');
            scale *= 10;
        }
        numerator.multiply_add(scale, chunk);
    }
    denominator.set(1);
    long long powers = exponent < 0 ? -exponent : exponent;
    BigNumber& scaled = exponent < 0 ? denominator : numerator;
    while (powers > 0) {
        u32 const step = powers >= 9 ? 1000000000u : 0;
        if (step) {
            scaled.multiply_add(step, 0);
            powers -= 9;
        } else {
            u32 factor = 1;
            for (long long i = 0; i < powers; ++i)
                factor *= 10;
            scaled.multiply_add(factor, 0);
            powers = 0;
        }
    }

    // Long division, in binary: lined up first, so that what's being divided is between a half and two, then a bit at a time. Sixty-four of those is
    // plenty, and what's left over only matters for whether there's any.
    int const shift = denominator.bit_length() - numerator.bit_length();
    if (shift > 0)
        numerator.shift_left(shift);
    else
        denominator.shift_left(-shift);
    u64 quotient = 0;
    for (int bit = 0; bit < 64; ++bit) {
        quotient <<= 1;
        if (numerator.compare(denominator) >= 0) {
            numerator.subtract(denominator);
            quotient |= 1;
        }
        numerator.shift_left(1);
    }
    bool const remainder = numerator.count != 0 || more;
    return make_number(quotient, -shift - 63, remainder, mantissa_bits, out_of_range);
}

}
