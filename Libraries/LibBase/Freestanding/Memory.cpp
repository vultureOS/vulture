/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Types.h>

// The four functions the compiler may call on its own (for struct copies and
// zero-initialization). Only linked into freestanding parts; hosted code gets
// them from the C library. -ffreestanding stops the compiler from turning
// these loops back into calls to themselves.

extern "C" {

void* memcpy(void* __restrict destination, void const* __restrict source, size_t count)
{
    auto* out = static_cast<u8*>(destination);
    auto const* in = static_cast<u8 const*>(source);
    for (size_t i = 0; i < count; ++i)
        out[i] = in[i];
    return destination;
}

void* memmove(void* destination, void const* source, size_t count)
{
    auto* out = static_cast<u8*>(destination);
    auto const* in = static_cast<u8 const*>(source);
    if (out < in) {
        for (size_t i = 0; i < count; ++i)
            out[i] = in[i];
    } else {
        for (size_t i = count; i > 0; --i)
            out[i - 1] = in[i - 1];
    }
    return destination;
}

void* memset(void* destination, int value, size_t count)
{
    auto* out = static_cast<u8*>(destination);
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<u8>(value);
    return destination;
}

int memcmp(void const* a, void const* b, size_t count)
{
    auto const* left = static_cast<u8 const*>(a);
    auto const* right = static_cast<u8 const*>(b);
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i])
            return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}
}
