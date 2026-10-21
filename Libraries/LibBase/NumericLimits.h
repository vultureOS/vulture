/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/TypeTraits.h>

namespace Base {

template<typename T>
struct NumericLimits;

template<Integral T>
requires(!IsSame<RemoveCV<T>, bool>)
struct NumericLimits<T> {
    static constexpr T max()
    {
        using Unsigned = MakeUnsigned<T>;
        if constexpr (IsSigned<T>)
            return static_cast<T>(static_cast<Unsigned>(~Unsigned(0)) >> 1);
        else
            return static_cast<T>(~T(0));
    }

    static constexpr T min()
    {
        if constexpr (IsSigned<T>)
            return -max() - 1;
        else
            return 0;
    }

    static constexpr size_t bits = sizeof(T) * 8;
};

}

using Base::NumericLimits;
