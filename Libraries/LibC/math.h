/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

#define M_E 2.71828182845904523536
#define M_LOG2E 1.44269504088896340736
#define M_LOG10E 0.434294481903251827651
#define M_LN2 0.693147180559945309417
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.785398163397448309616
#define M_1_PI 0.318309886183790671538
#define M_2_PI 0.636619772367581343076
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.707106781186547524401

#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double fabs(double);
double sqrt(double);
double cbrt(double);
double hypot(double, double);
double floor(double);
double ceil(double);
double trunc(double);
double round(double);
long lround(double);
double fmod(double, double);
double modf(double, double* whole);
double frexp(double, int* exponent);
double ldexp(double, int exponent);
double copysign(double magnitude, double sign);
double fmin(double, double);
double fmax(double, double);
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double y, double x);
double sinh(double);
double cosh(double);
double tanh(double);
double exp(double);
double log(double);
double log2(double);
double log10(double);
double pow(double, double);

float fabsf(float);
float sqrtf(float);
float cbrtf(float);
float hypotf(float, float);
float floorf(float);
float ceilf(float);
float truncf(float);
float roundf(float);
long lroundf(float);
float fmodf(float, float);
float copysignf(float magnitude, float sign);
float fminf(float, float);
float fmaxf(float, float);
float sinf(float);
float cosf(float);
float tanf(float);
float asinf(float);
float acosf(float);
float atanf(float);
float atan2f(float y, float x);
float expf(float);
float logf(float);
float log2f(float);
float log10f(float);
float powf(float, float);

__END_DECLS
