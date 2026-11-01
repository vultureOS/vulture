/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using Constructor = void (*)(int, char**, char**);
using Destructor = void (*)();
extern "C" Constructor __init_array_start[] __attribute__((weak));
extern "C" Constructor __init_array_end[] __attribute__((weak));
extern "C" Destructor __fini_array_start[] __attribute__((weak));
extern "C" Destructor __fini_array_end[] __attribute__((weak));
// Defined by the linker in a shared object (libSystem.B.dylib): there, the
// dynamic linker runs every object's initializers and finalizers. In a
// statically linked program it is not there, and this file runs them.
extern "C" char _DYNAMIC[] __attribute__((weak, visibility("hidden")));

extern "C" int main(int argc, char** argv, char** envp);

extern "C" {

int errno;
char** environ;

void __stdio_flush_all();
}

namespace {

constexpr int max_exit_handlers = 64;

struct ExitHandler {
    void (*function)(void*);
    void* argument;
};

ExitHandler s_exit_handlers[max_exit_handlers];
int s_exit_handler_count = 0;

}

// Shared by atexit() and the C++ runtime's __cxa_atexit().
extern "C" int __libc_register_exit_handler(void (*function)(void*), void* argument)
{
    if (s_exit_handler_count == max_exit_handlers)
        return -1;
    s_exit_handlers[s_exit_handler_count++] = { function, argument };
    return 0;
}

// The C library's own setup, before any other initializer (the dynamic
// linker calls it, like every initializer, with the arguments and
// environment).
extern "C" void __libc_initialize(int argc, char** argv, char** envp)
{
    environ = envp;
    if (argc > 0 && argv && argv[0])
        setprogname(argv[0]);
}

__attribute__((section(".init_array.00001"), used)) static Constructor s_initialize = __libc_initialize;

extern "C" [[noreturn]] void __libc_start(unsigned long* stack)
{
    int argc = static_cast<int>(stack[0]);
    char** argv = reinterpret_cast<char**>(stack + 1);
    char** envp = argv + argc + 1;
    if (!_DYNAMIC) {
        __libc_initialize(argc, argv, envp);
        if (__init_array_start) {
            for (auto* constructor = __init_array_start; constructor != __init_array_end; ++constructor)
                (*constructor)(argc, argv, envp);
        }
    }
    exit(main(argc, argv, environ));
}

extern "C" {

int atexit(void (*function)())
{
    return __libc_register_exit_handler([](void* argument) { reinterpret_cast<void (*)()>(argument)(); }, reinterpret_cast<void*>(function));
}

void exit(int status)
{
    // In reverse order of registration, then destructors and buffers.
    while (s_exit_handler_count > 0) {
        auto handler = s_exit_handlers[--s_exit_handler_count];
        handler.function(handler.argument);
    }
    if (!_DYNAMIC && __fini_array_start) {
        for (auto* destructor = __fini_array_end; destructor != __fini_array_start;)
            (*--destructor)();
    }
    __stdio_flush_all();
    _exit(status);
}

void _exit(int status)
{
    System::syscall(System::Syscall::exit, static_cast<unsigned long>(status));
    __builtin_trap();
}

void _Exit(int status)
{
    _exit(status);
}

void abort()
{
    __stdio_flush_all();
    raise(SIGABRT);
    // If SIGABRT was caught or ignored, end the process anyway.
    _exit(127);
}

void __assertion_failed(char const* expression, char const* file, unsigned line, char const* function)
{
    fprintf(stderr, "ASSERTION FAILED: %s\n  at %s:%u in %s()\n", expression, file, line, function);
    abort();
}
}
