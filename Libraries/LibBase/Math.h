/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/StdLibExtras.h>
#include <LibBase/Types.h>

// Elementary functions, the same code on the host and in the OS (so a host
// tool and the system draw the same pixels). <math.h> is built on these.
//
// Each reduces its argument to a small range and evaluates a Taylor series
// there, to within a few units in the last place of a double.

namespace Base::Math {

inline constexpr double pi = 3.141592653589793238462643383279502884;
inline constexpr double e = 2.718281828459045235360287471352662498;
inline constexpr double ln2 = 0.693147180559945309417232121458176568;
inline constexpr double ln10 = 2.302585092994045684017991454684364208;

[[nodiscard]] constexpr u64 bits_of(double value) { return __builtin_bit_cast(u64, value); }
[[nodiscard]] constexpr double double_from_bits(u64 bits) { return __builtin_bit_cast(double, bits); }

[[nodiscard]] constexpr bool is_nan(double value) { return value != value; }
[[nodiscard]] constexpr bool is_infinite(double value) { return (bits_of(value) & ~(1ull << 63)) == 0x7ff0000000000000ull; }
[[nodiscard]] constexpr bool is_finite(double value) { return (bits_of(value) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull; }
[[nodiscard]] constexpr bool sign_bit(double value) { return bits_of(value) >> 63; }
[[nodiscard]] constexpr double infinity() { return double_from_bits(0x7ff0000000000000ull); }
[[nodiscard]] constexpr double quiet_nan() { return double_from_bits(0x7ff8000000000000ull); }

[[nodiscard]] constexpr double absolute(double value) { return double_from_bits(bits_of(value) & ~(1ull << 63)); }
[[nodiscard]] constexpr float absolute(float value) { return __builtin_bit_cast(float, __builtin_bit_cast(u32, value) & 0x7fffffffu); }
[[nodiscard]] constexpr int absolute(int value) { return value < 0 ? -value : value; }
[[nodiscard]] constexpr double copy_sign(double magnitude, double sign)
{
    return double_from_bits((bits_of(magnitude) & ~(1ull << 63)) | (bits_of(sign) & (1ull << 63)));
}

[[nodiscard]] inline double square_root(double value) { return __builtin_sqrt(value); }
[[nodiscard]] inline float square_root(float value) { return __builtin_sqrtf(value); }

// Beyond 2^52 every double is a whole number.
[[nodiscard]] constexpr double truncate(double value)
{
    if (!(absolute(value) < 4503599627370496.0))
        return value;
    return copy_sign(static_cast<double>(static_cast<i64>(value)), value);
}

[[nodiscard]] constexpr double floor(double value)
{
    double whole = truncate(value);
    return whole > value ? whole - 1.0 : whole;
}

[[nodiscard]] constexpr double ceil(double value)
{
    double whole = truncate(value);
    return whole < value ? copy_sign(whole + 1.0, value) : whole;
}

// Halves go away from zero.
[[nodiscard]] constexpr double round(double value)
{
    double whole = truncate(value);
    return absolute(value - whole) >= 0.5 ? whole + copy_sign(1.0, value) : whole;
}

[[nodiscard]] constexpr float floor(float value)
{
    if (!(absolute(value) < 8388608.0f))
        return value;
    float whole = static_cast<float>(static_cast<i32>(value));
    return whole > value ? whole - 1.0f : whole;
}

[[nodiscard]] constexpr float ceil(float value)
{
    if (!(absolute(value) < 8388608.0f))
        return value;
    float whole = static_cast<float>(static_cast<i32>(value));
    return whole < value ? whole + 1.0f : whole;
}

[[nodiscard]] constexpr float round(float value)
{
    return value < 0 ? ceil(value - 0.5f) : floor(value + 0.5f);
}

// `value` scaled by 2^exponent, in as many steps as the range needs.
[[nodiscard]] constexpr double scale_by_power_of_two(double value, int exponent)
{
    while (exponent > 1023) {
        value *= 8.98846567431157953865e+307; // 2^1023
        exponent -= 1023;
        if (is_infinite(value))
            return value;
    }
    while (exponent < -1022) {
        value *= 2.22507385850720138309e-308; // 2^-1022
        exponent += 1022;
        if (value == 0)
            return value;
    }
    return value * double_from_bits(static_cast<u64>(exponent + 1023) << 52);
}

// value = mantissa * 2^exponent, the mantissa in [0.5, 1).
[[nodiscard]] constexpr double split_exponent(double value, int& exponent)
{
    exponent = 0;
    if (value == 0 || !is_finite(value))
        return value;
    if (absolute(value) < 2.22507385850720138309e-308) {
        value *= 18014398509481984.0; // 2^54: out of the subnormals.
        exponent = -54;
    }
    u64 bits = bits_of(value);
    exponent += static_cast<int>((bits >> 52) & 0x7ff) - 1022;
    return double_from_bits((bits & ~(0x7ffull << 52)) | (1022ull << 52));
}

[[nodiscard]] constexpr double modulo(double x, double y)
{
    if (is_nan(x) || is_nan(y) || is_infinite(x) || y == 0)
        return quiet_nan();
    if (is_infinite(y))
        return x;
    double dividend = absolute(x);
    double divisor = absolute(y);
    if (dividend < divisor)
        return x;
    int dividend_exponent = 0, divisor_exponent = 0;
    (void)split_exponent(dividend, dividend_exponent);
    (void)split_exponent(divisor, divisor_exponent);
    // Long division: subtract the divisor at each power of two, top down.
    for (int shift = dividend_exponent - divisor_exponent; shift >= 0; --shift) {
        double scaled = scale_by_power_of_two(divisor, shift);
        if (dividend >= scaled)
            dividend -= scaled;
    }
    return copy_sign(dividend, x);
}

namespace Detail {

// sin(x) for |x| <= pi/4.
[[nodiscard]] constexpr double sine_series(double x)
{
    double x2 = x * x;
    return x + x * x2 * (-1.0 / 6 + x2 * (1.0 / 120 + x2 * (-1.0 / 5040 + x2 * (1.0 / 362880 + x2 * (-1.0 / 39916800 + x2 * (1.0 / 6227020800.0 + x2 * (-1.0 / 1307674368000.0)))))));
}

// cos(x) for |x| <= pi/4.
[[nodiscard]] constexpr double cosine_series(double x)
{
    double x2 = x * x;
    return 1.0 + x2 * (-1.0 / 2 + x2 * (1.0 / 24 + x2 * (-1.0 / 720 + x2 * (1.0 / 40320 + x2 * (-1.0 / 3628800 + x2 * (1.0 / 479001600.0 + x2 * (-1.0 / 87178291200.0)))))));
}

// x minus the nearest multiple of pi/2 (in two parts, so little is lost),
// and that multiple modulo 4. Good while the multiple fits well in a double.
constexpr double reduce_to_quadrant(double x, int& quadrant)
{
    constexpr double two_over_pi = 0.636619772367581343075535053490057448;
    constexpr double pi_over_two_high = 1.57079632673412561417e+00;
    constexpr double pi_over_two_low = 6.07710050650619224932e-11;
    double multiple = round(x * two_over_pi);
    quadrant = static_cast<int>(modulo(multiple, 4.0));
    if (quadrant < 0)
        quadrant += 4;
    return (x - multiple * pi_over_two_high) - multiple * pi_over_two_low;
}

}

[[nodiscard]] constexpr double sine(double x)
{
    if (!is_finite(x))
        return quiet_nan();
    int quadrant = 0;
    double r = Detail::reduce_to_quadrant(x, quadrant);
    switch (quadrant) {
    case 0:
        return Detail::sine_series(r);
    case 1:
        return Detail::cosine_series(r);
    case 2:
        return -Detail::sine_series(r);
    default:
        return -Detail::cosine_series(r);
    }
}

[[nodiscard]] constexpr double cosine(double x)
{
    if (!is_finite(x))
        return quiet_nan();
    int quadrant = 0;
    double r = Detail::reduce_to_quadrant(x, quadrant);
    switch (quadrant) {
    case 0:
        return Detail::cosine_series(r);
    case 1:
        return -Detail::sine_series(r);
    case 2:
        return -Detail::cosine_series(r);
    default:
        return Detail::sine_series(r);
    }
}

[[nodiscard]] constexpr double tangent(double x)
{
    return sine(x) / cosine(x);
}

[[nodiscard]] constexpr double arc_tangent(double x)
{
    if (is_nan(x))
        return x;
    if (x < 0)
        return -arc_tangent(-x);
    if (x > 1)
        return pi / 2 - arc_tangent(1 / x);
    // Down to |t| <= tan(pi/24) around one of these: atan(x) = atan(c) + atan(t).
    double base = 0;
    double t = x;
    if (x > 0.76732698797896) { // tan(3pi/16 + pi/64)-ish: nearest of three points.
        t = (x - 1) / (x + 1);
        base = pi / 4;
    } else if (x > 0.26794919243112) { // tan(pi/12)
        constexpr double tan_pi_over_6 = 0.57735026918962576451;
        t = (x - tan_pi_over_6) / (1 + x * tan_pi_over_6);
        base = pi / 6;
    }
    double t2 = t * t;
    double sum = 0;
    for (int n = 25; n >= 1; n -= 2)
        sum = 1.0 / n - t2 * sum;
    return base + t * sum;
}

[[nodiscard]] constexpr double arc_tangent2(double y, double x)
{
    if (is_nan(x) || is_nan(y))
        return quiet_nan();
    if (x == 0 && y == 0)
        return sign_bit(x) ? copy_sign(pi, y) : y;
    if (is_infinite(x) && is_infinite(y))
        return copy_sign(sign_bit(x) ? 3 * pi / 4 : pi / 4, y);
    if (x == 0 || is_infinite(y))
        return copy_sign(pi / 2, y);
    if (is_infinite(x))
        return sign_bit(x) ? copy_sign(pi, y) : copy_sign(0.0, y);
    double angle = arc_tangent(absolute(y / x));
    if (sign_bit(x))
        angle = pi - angle;
    return copy_sign(angle, y);
}

[[nodiscard]] constexpr double exponential(double x)
{
    if (is_nan(x))
        return x;
    if (x > 709.782712893384)
        return infinity();
    if (x < -745.133219101941)
        return 0;
    constexpr double ln2_high = 6.93147180369123816490e-01;
    constexpr double ln2_low = 1.90821492927058770002e-10;
    double multiple = round(x / ln2);
    double r = (x - multiple * ln2_high) - multiple * ln2_low;
    double sum = 1;
    for (int n = 14; n >= 1; --n)
        sum = 1 + r * sum / n;
    return scale_by_power_of_two(sum, static_cast<int>(multiple));
}

[[nodiscard]] constexpr double natural_log(double x)
{
    if (is_nan(x) || x < 0)
        return quiet_nan();
    if (x == 0)
        return -infinity();
    if (is_infinite(x))
        return x;
    int exponent = 0;
    double mantissa = split_exponent(x, exponent);
    if (mantissa < 0.70710678118654752440) {
        mantissa *= 2;
        --exponent;
    }
    // ln(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716.
    double s = (mantissa - 1) / (mantissa + 1);
    double s2 = s * s;
    double sum = 0;
    for (int n = 21; n >= 1; n -= 2)
        sum = 1.0 / n + s2 * sum;
    constexpr double ln2_high = 6.93147180369123816490e-01;
    constexpr double ln2_low = 1.90821492927058770002e-10;
    return exponent * ln2_high + (2 * s * sum + exponent * ln2_low);
}

[[nodiscard]] constexpr double log2(double x) { return natural_log(x) / ln2; }
[[nodiscard]] constexpr double log10(double x) { return natural_log(x) / ln10; }

[[nodiscard]] constexpr double power(double base, double exponent)
{
    if (exponent == 0 || base == 1)
        return 1;
    if (is_nan(base) || is_nan(exponent))
        return quiet_nan();
    bool is_whole = truncate(exponent) == exponent;
    if (is_whole && absolute(exponent) <= 1024) {
        // Exactly, by squaring: pow(2, 10) is 1024, not nearly.
        double result = 1;
        double square = base;
        for (i64 left = static_cast<i64>(absolute(exponent)); left; left >>= 1) {
            if (left & 1)
                result *= square;
            square *= square;
        }
        return exponent < 0 ? 1 / result : result;
    }
    bool odd = is_whole && absolute(exponent) < 9007199254740992.0 && modulo(exponent, 2.0) != 0;
    if (base == 0) {
        double zero_result = exponent < 0 ? infinity() : 0.0;
        return odd ? copy_sign(zero_result, base) : zero_result;
    }
    if (base < 0 && !is_whole)
        return quiet_nan();
    double magnitude = exponential(exponent * natural_log(absolute(base)));
    return base < 0 && odd ? -magnitude : magnitude;
}

[[nodiscard]] inline double arc_sine(double x)
{
    return arc_tangent2(x, square_root((1 - x) * (1 + x)));
}

[[nodiscard]] inline double arc_cosine(double x)
{
    return arc_tangent2(square_root((1 - x) * (1 + x)), x);
}

[[nodiscard]] inline double hypotenuse(double x, double y)
{
    x = absolute(x);
    y = absolute(y);
    if (is_infinite(x) || is_infinite(y))
        return infinity();
    if (x < y)
        swap(x, y);
    if (x == 0)
        return y;
    double ratio = y / x;
    return x * square_root(1 + ratio * ratio);
}

[[nodiscard]] constexpr double cube_root(double x)
{
    if (x == 0 || !is_finite(x))
        return x;
    double guess = copy_sign(exponential(natural_log(absolute(x)) / 3), x);
    // Newton's method polishes the last bits.
    guess -= (guess * guess * guess - x) / (3 * guess * guess);
    guess -= (guess * guess * guess - x) / (3 * guess * guess);
    return guess;
}

[[nodiscard]] constexpr double hyperbolic_sine(double x)
{
    double grown = exponential(absolute(x));
    return copy_sign((grown - 1 / grown) / 2, x);
}

[[nodiscard]] constexpr double hyperbolic_cosine(double x)
{
    double grown = exponential(absolute(x));
    return (grown + 1 / grown) / 2;
}

[[nodiscard]] constexpr double hyperbolic_tangent(double x)
{
    if (absolute(x) > 22)
        return copy_sign(1.0, x);
    double grown = exponential(2 * absolute(x));
    return copy_sign((grown - 1) / (grown + 1), x);
}

}
