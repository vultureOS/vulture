/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibBase/New.h>
#include <LibTest/TestCase.h>

// Host tests are built without system headers or the C++ standard library.
// The few C library functions they need are declared here, and this file
// supplies the C++ runtime hooks that libc++abi would normally provide.

extern "C" {
ssize_t write(int fd, void const* buffer, size_t count);
[[noreturn]] void abort();
void* malloc(size_t size);
void free(void* pointer);
}

void Test::print(StringView text)
{
    size_t written = 0;
    while (written < text.length()) {
        ssize_t result = write(1, text.characters_without_null_termination() + written, text.length() - written);
        if (result <= 0)
            return;
        written += static_cast<size_t>(result);
    }
}

void Base::verification_failed(char const* expression, char const* file, unsigned line)
{
    FixedStringBuilder<512> message;
    format_to(message, "VERIFY failed: {} at {}:{}\n", expression, file, line);
    Test::print(message.view());
    abort();
}

void* operator new(size_t size)
{
    if (void* pointer = malloc(size ? size : 1))
        return pointer;
    abort();
}

void* operator new[](size_t size)
{
    return operator new(size);
}

void operator delete(void* pointer) noexcept
{
    free(pointer);
}

void operator delete(void* pointer, size_t) noexcept
{
    free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept
{
    free(pointer);
}

void* operator new(size_t size, Base::NoThrowTag) noexcept
{
    return malloc(size ? size : 1);
}

void* operator new[](size_t size, Base::NoThrowTag) noexcept
{
    return malloc(size ? size : 1);
}

void operator delete(void* pointer, Base::NoThrowTag) noexcept
{
    free(pointer);
}

void operator delete[](void* pointer, Base::NoThrowTag) noexcept
{
    free(pointer);
}

extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    abort();
}
