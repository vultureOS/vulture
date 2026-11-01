/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <math.h>

// <math.h>, on LibBase's functions. As on macOS, errors are not reported
// through errno.

namespace Math = Base::Math;

extern "C" {

double fabs(double x) { return Math::absolute(x); }
double sqrt(double x) { return Math::square_root(x); }
double cbrt(double x) { return Math::cube_root(x); }
double hypot(double x, double y) { return Math::hypotenuse(x, y); }
double floor(double x) { return Math::floor(x); }
double ceil(double x) { return Math::ceil(x); }
double trunc(double x) { return Math::truncate(x); }
double round(double x) { return Math::round(x); }
long lround(double x) { return static_cast<long>(Math::round(x)); }
double fmod(double x, double y) { return Math::modulo(x, y); }
double frexp(double x, int* exponent) { return Math::split_exponent(x, *exponent); }
double ldexp(double x, int exponent) { return Math::scale_by_power_of_two(x, exponent); }
double copysign(double magnitude, double sign) { return Math::copy_sign(magnitude, sign); }
double sin(double x) { return Math::sine(x); }
double cos(double x) { return Math::cosine(x); }
double tan(double x) { return Math::tangent(x); }
double asin(double x) { return Math::arc_sine(x); }
double acos(double x) { return Math::arc_cosine(x); }
double atan(double x) { return Math::arc_tangent(x); }
double atan2(double y, double x) { return Math::arc_tangent2(y, x); }
double sinh(double x) { return Math::hyperbolic_sine(x); }
double cosh(double x) { return Math::hyperbolic_cosine(x); }
double tanh(double x) { return Math::hyperbolic_tangent(x); }
double exp(double x) { return Math::exponential(x); }
double log(double x) { return Math::natural_log(x); }
double log2(double x) { return Math::log2(x); }
double log10(double x) { return Math::log10(x); }
double pow(double x, double y) { return Math::power(x, y); }

double modf(double x, double* whole)
{
    *whole = Math::truncate(x);
    return Math::is_infinite(x) ? Math::copy_sign(0.0, x) : Math::copy_sign(x - *whole, x);
}

double fmin(double a, double b)
{
    if (Math::is_nan(a))
        return b;
    if (Math::is_nan(b))
        return a;
    return a < b ? a : b;
}

double fmax(double a, double b)
{
    if (Math::is_nan(a))
        return b;
    if (Math::is_nan(b))
        return a;
    return a > b ? a : b;
}

float fabsf(float x) { return Math::absolute(x); }
float sqrtf(float x) { return Math::square_root(x); }
float cbrtf(float x) { return static_cast<float>(cbrt(x)); }
float hypotf(float x, float y) { return static_cast<float>(hypot(x, y)); }
float floorf(float x) { return Math::floor(x); }
float ceilf(float x) { return Math::ceil(x); }
float truncf(float x) { return static_cast<float>(trunc(x)); }
float roundf(float x) { return static_cast<float>(round(x)); }
long lroundf(float x) { return lround(x); }
float fmodf(float x, float y) { return static_cast<float>(fmod(x, y)); }
float copysignf(float magnitude, float sign) { return static_cast<float>(copysign(magnitude, sign)); }
float fminf(float a, float b) { return static_cast<float>(fmin(a, b)); }
float fmaxf(float a, float b) { return static_cast<float>(fmax(a, b)); }
float sinf(float x) { return static_cast<float>(sin(x)); }
float cosf(float x) { return static_cast<float>(cos(x)); }
float tanf(float x) { return static_cast<float>(tan(x)); }
float asinf(float x) { return static_cast<float>(asin(x)); }
float acosf(float x) { return static_cast<float>(acos(x)); }
float atanf(float x) { return static_cast<float>(atan(x)); }
float atan2f(float y, float x) { return static_cast<float>(atan2(y, x)); }
float expf(float x) { return static_cast<float>(exp(x)); }
float logf(float x) { return static_cast<float>(log(x)); }
float log2f(float x) { return static_cast<float>(log2(x)); }
float log10f(float x) { return static_cast<float>(log10(x)); }
float powf(float x, float y) { return static_cast<float>(pow(x, y)); }
}
