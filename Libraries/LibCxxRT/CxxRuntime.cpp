/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibBase/New.h>
#include <stdio.h>
#include <stdlib.h>

// The C++ runtime support programs need: allocation through malloc, global
// destructors through the C library's exit handlers, and the hooks the
// compiler and LibBase call when something goes wrong.

extern "C" int __libc_register_exit_handler(void (*function)(void*), void* argument);

extern "C" {

int __cxa_atexit(void (*destructor)(void*), void* object, void*)
{
    return __libc_register_exit_handler(destructor, object);
}

[[noreturn]] void __cxa_pure_virtual()
{
    fputs("pure virtual function called\n", stderr);
    abort();
}
}

void Base::verification_failed(char const* expression, char const* file, unsigned line)
{
    fprintf(stderr, "VERIFY failed: %s at %s:%u\n", expression, file, line);
    abort();
}

// Plain `new` cannot report failure (without exceptions); it ends the
// program. try_make() and `new (nothrow)` return null instead.
void* operator new(size_t size)
{
    if (void* pointer = malloc(size))
        return pointer;
    fputs("operator new: out of memory\n", stderr);
    abort();
}

void* operator new[](size_t size)
{
    return operator new(size);
}

void* operator new(size_t size, Base::NoThrowTag) noexcept
{
    return malloc(size);
}

void* operator new[](size_t size, Base::NoThrowTag) noexcept
{
    return malloc(size);
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

void operator delete(void* pointer, Base::NoThrowTag) noexcept
{
    free(pointer);
}

void operator delete[](void* pointer, Base::NoThrowTag) noexcept
{
    free(pointer);
}
