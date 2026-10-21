/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// Type traits and concepts, implemented with clang's type-trait builtins.

namespace Base {

template<typename T, T v>
struct IntegralConstant {
    static constexpr T value = v;
    using ValueType = T;
    constexpr operator ValueType() const { return value; }
};

using FalseType = IntegralConstant<bool, false>;
using TrueType = IntegralConstant<bool, true>;

template<typename T>
using RemoveReference = __remove_reference_t(T);
template<typename T>
using RemoveConst = __remove_const(T);
template<typename T>
using RemoveVolatile = __remove_volatile(T);
template<typename T>
using RemoveCV = __remove_cv(T);
template<typename T>
using RemoveCVReference = __remove_cvref(T);
template<typename T>
using MakeUnsigned = __make_unsigned(T);
template<typename T>
using MakeSigned = __make_signed(T);
template<typename T>
using UnderlyingType = __underlying_type(T);
template<typename T>
using Decay = __decay(T);

template<typename T>
struct TypeIdentityHelper {
    using Type = T;
};

// Blocks template argument deduction through a parameter.
template<typename T>
using TypeIdentity = typename TypeIdentityHelper<T>::Type;

template<bool condition, typename TrueT, typename FalseT>
struct ConditionalHelper {
    using Type = TrueT;
};

template<typename TrueT, typename FalseT>
struct ConditionalHelper<false, TrueT, FalseT> {
    using Type = FalseT;
};

template<bool condition, typename TrueT, typename FalseT>
using Conditional = typename ConditionalHelper<condition, TrueT, FalseT>::Type;

template<typename T, typename U>
inline constexpr bool IsSame = __is_same(T, U);
template<typename T>
inline constexpr bool IsIntegral = __is_integral(T);
template<typename T>
inline constexpr bool IsFloatingPoint = __is_floating_point(T);
template<typename T>
inline constexpr bool IsSigned = __is_signed(T);
template<typename T>
inline constexpr bool IsUnsigned = __is_unsigned(T);
template<typename T>
inline constexpr bool IsEnum = __is_enum(T);
template<typename T>
inline constexpr bool IsPointer = __is_pointer(T);
template<typename T>
inline constexpr bool IsConst = __is_const(T);
template<typename T>
inline constexpr bool IsLvalueReference = __is_lvalue_reference(T);
template<typename T>
inline constexpr bool IsRvalueReference = __is_rvalue_reference(T);
template<typename T>
inline constexpr bool IsArray = __is_array(T);
template<typename T>
inline constexpr bool IsTriviallyCopyable = __is_trivially_copyable(T);
template<typename T>
inline constexpr bool IsTriviallyDestructible = __is_trivially_destructible(T);
template<typename T, typename... Args>
inline constexpr bool IsConstructible = __is_constructible(T, Args...);
template<typename From, typename To>
inline constexpr bool IsConvertible = __is_convertible(From, To);
template<typename BaseT, typename DerivedT>
inline constexpr bool IsBaseOf = __is_base_of(BaseT, DerivedT);

template<typename T>
concept Integral = IsIntegral<T>;
template<typename T>
concept SignedIntegral = IsIntegral<T> && IsSigned<T>;
template<typename T>
concept UnsignedIntegral = IsIntegral<T> && IsUnsigned<T>;
template<typename T>
concept Enum = IsEnum<T>;
template<typename T, typename U>
concept SameAs = IsSame<T, U>;
template<typename From, typename To>
concept ConvertibleTo = IsConvertible<From, To>;
template<typename T, typename... Args>
concept ConstructibleFrom = IsConstructible<T, Args...>;

}
