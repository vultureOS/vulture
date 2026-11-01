/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/thread.h>
#include <LibBase/Assertions.h>
#include <LibBase/New.h>
#include <LibSystem/Syscall.h>
#include <stdio.h>
#include <stdlib.h>

// The C++ runtime support programs need: allocation through malloc, global
// and thread_local destructors through the C library's exit handlers,
// thread-safe initialization of function-local statics, and the hooks the
// compiler and LibBase call when something goes wrong.

extern "C" int __libc_register_exit_handler(void (*function)(void*), void* argument);
extern "C" int __libc_register_thread_exit_handler(void (*function)(void*), void* argument);

extern "C" {

int __cxa_atexit(void (*destructor)(void*), void* object, void*)
{
    return __libc_register_exit_handler(destructor, object);
}

int __cxa_thread_atexit(void (*destructor)(void*), void* object, void*)
{
    return __libc_register_thread_exit_handler(destructor, object);
}

// Function-local statics. A guard is 64 bits: the compiler reads its first
// byte to see whether the object is ready; the second word is for us, 0
// free, 1 being initialized, 2 with others waiting.
static unsigned* guard_state(unsigned long long* guard)
{
    return reinterpret_cast<unsigned*>(guard) + 1;
}

static bool guard_is_done(unsigned long long* guard)
{
    return __atomic_load_n(reinterpret_cast<u8*>(guard), __ATOMIC_ACQUIRE) != 0;
}

int __cxa_guard_acquire(unsigned long long* guard)
{
    auto* state = guard_state(guard);
    for (;;) {
        if (guard_is_done(guard))
            return 0;
        unsigned expected = 0;
        if (__atomic_compare_exchange_n(state, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            // Finished between the look and taking it?
            if (guard_is_done(guard)) {
                __atomic_store_n(state, 0u, __ATOMIC_RELEASE);
                return 0;
            }
            return 1;
        }
        if (expected == 1 && !__atomic_compare_exchange_n(state, &expected, 2u, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            continue;
        System::syscall(System::Syscall::futex, reinterpret_cast<unsigned long>(state), VULTURE_FUTEX_WAIT, 2, 0);
    }
}

static void guard_finish(unsigned long long* guard)
{
    auto* state = guard_state(guard);
    if (__atomic_exchange_n(state, 0u, __ATOMIC_RELEASE) == 2)
        System::syscall(System::Syscall::futex, reinterpret_cast<unsigned long>(state), VULTURE_FUTEX_WAKE, 0x7fffffff);
}

void __cxa_guard_release(unsigned long long* guard)
{
    __atomic_store_n(reinterpret_cast<u8*>(guard), u8(1), __ATOMIC_RELEASE);
    guard_finish(guard);
}

void __cxa_guard_abort(unsigned long long* guard)
{
    guard_finish(guard);
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
