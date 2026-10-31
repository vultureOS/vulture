/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/MemoryLayout.h>
#include <LibBase/Error.h>
#include <LibBase/Optional.h>

// The only way the kernel touches memory a program names. Addresses are
// checked to lie in user space (a program must never get the kernel to
// read or write kernel memory for it), and a fault on the way (unmapped,
// or not writable) becomes EFAULT instead of a panic. Pages not yet
// present are faulted in as usual. Never call with a spin lock held.

namespace Kernel {

[[nodiscard]] bool is_user_range(FlatPtr address, size_t size);

ErrorOr<void> copy_from_user(void* destination, FlatPtr source, size_t size);
ErrorOr<void> copy_to_user(FlatPtr destination, void const* source, size_t size);

template<typename T>
ErrorOr<T> copy_from_user(FlatPtr source)
{
    T value;
    TRY(copy_from_user(&value, source, sizeof(T)));
    return value;
}

template<typename T>
ErrorOr<void> copy_to_user(FlatPtr destination, T const& value)
{
    return copy_to_user(destination, &value, sizeof(T));
}

// Copies a NUL-terminated string into `buffer` (`capacity` bytes, including
// the terminator) and returns its length; ENAMETOOLONG if it does not fit.
ErrorOr<size_t> copy_string_from_user(FlatPtr source, char* buffer, size_t capacity);

// For the exception handler: where to resume after a fault at `pc`, if
// `pc` is one of the user access instructions.
[[nodiscard]] Optional<FlatPtr> user_access_fixup(FlatPtr pc);

}
