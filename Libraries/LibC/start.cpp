/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Lock.h"
#include <Kernel/API/POSIX/auxv.h>
#include <Kernel/API/POSIX/memory.h>
#include <LibELF/ELF.h>
#include <LibSystem/Syscall.h>
#include <LibSystem/ThreadPointer.h>
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

thread_local int __libc_errno;
char** environ;

void __stdio_flush_all();
void __pthread_initialize();
void __libc_run_thread_exit_handlers();

int* __error(void)
{
    return &__libc_errno;
}
}

namespace {

constexpr int max_exit_handlers = 64;

struct ExitHandler {
    void (*function)(void*);
    void* argument;
};

ExitHandler s_exit_handlers[max_exit_handlers];
int s_exit_handler_count = 0;
LibC::Lock s_exit_handlers_lock;

// In a statically linked program there is no dyld to set up the first
// thread's thread-local storage. The usual size needs no system call.
System::ThreadLocalImage s_tls_image;
alignas(64) u8 s_small_tls_image[512];
alignas(64) u8 s_small_tls_block[512];

void set_up_static_thread_local_storage(unsigned long* stack)
{
    int argc = static_cast<int>(stack[0]);
    char** envp = reinterpret_cast<char**>(stack + 1) + argc + 1;
    while (*envp)
        ++envp;
    auto const* auxiliary = reinterpret_cast<unsigned long const*>(envp + 1);
    ELF::Elf64_Phdr const* headers = nullptr;
    unsigned long header_count = 0;
    for (; auxiliary[0] != AT_NULL; auxiliary += 2) {
        if (auxiliary[0] == AT_PHDR)
            headers = reinterpret_cast<ELF::Elf64_Phdr const*>(auxiliary[1]);
        else if (auxiliary[0] == AT_PHNUM)
            header_count = auxiliary[1];
    }
    ELF::Elf64_Phdr const* segment = nullptr;
    for (unsigned long i = 0; headers && i < header_count; ++i) {
        if (headers[i].p_type == ELF::PT_TLS)
            segment = &headers[i];
    }
    unsigned long alignment = 16;
    unsigned long end = 0;
    unsigned long offset = 0;
    if (segment) {
        unsigned long segment_alignment = segment->p_align ? segment->p_align : 1;
        unsigned long const place = System::next_tls_offset(0, segment->p_vaddr, segment->p_memsz, segment_alignment);
        offset = System::tls_pointer_offset(place);
#if defined(__x86_64__)
        end = place;
#else
        end = place + segment->p_memsz;
#endif
        alignment = segment_alignment > alignment ? segment_alignment : alignment;
    }
    unsigned long const size = System::tls_image_size(end, alignment);
    unsigned long const storage = System::thread_storage_size(size);
    u8* image = s_small_tls_image;
    u8* block = s_small_tls_block;
    if (size > sizeof(s_small_tls_image) || storage > sizeof(s_small_tls_block) || alignment > 64) {
        unsigned long room = (size + storage + alignment * 2 + 4095) & ~4095ul;
        long mapped = System::syscall(System::Syscall::mmap, 0, room, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, static_cast<unsigned long>(-1), 0);
        if (mapped < 0)
            __builtin_trap();
        image = reinterpret_cast<u8*>(mapped);
        block = reinterpret_cast<u8*>((static_cast<unsigned long>(mapped) + size + alignment - 1) & ~(alignment - 1));
    }
    if (segment) {
        auto const* source = reinterpret_cast<u8 const*>(segment->p_vaddr);
        unsigned long const place = System::tls_image_index(offset, size);
        for (unsigned long byte = 0; byte < segment->p_filesz; ++byte)
            image[place + byte] = source[byte];
    }
    s_tls_image = { image, size, alignment };
    System::set_thread_control_block(System::initialize_thread_storage(block, s_tls_image));
}

}

// Shared by atexit() and the C++ runtime's __cxa_atexit().
extern "C" int __libc_register_exit_handler(void (*function)(void*), void* argument)
{
    LibC::LockGuard guard(s_exit_handlers_lock);
    if (s_exit_handler_count == max_exit_handlers)
        return -1;
    s_exit_handlers[s_exit_handler_count++] = { function, argument };
    return 0;
}

// A library's going away. What it wanted done at exit gets done now, while it's still there to do it: anything of its, as
// the function or as what's given to it (which is where atexit()'s function is).
extern "C" void __libc_run_exit_handlers_in(unsigned long start, unsigned long size)
{
    auto const belongs = [&](void const* address) { return reinterpret_cast<unsigned long>(address) - start < size; };
    for (;;) {
        ExitHandler handler {};
        bool found = false;
        {
            LibC::LockGuard guard(s_exit_handlers_lock);
            for (int i = s_exit_handler_count - 1; i >= 0; --i) {
                if (!belongs(reinterpret_cast<void const*>(s_exit_handlers[i].function)) && !belongs(s_exit_handlers[i].argument))
                    continue;
                handler = s_exit_handlers[i];
                for (int n = i; n + 1 < s_exit_handler_count; ++n)
                    s_exit_handlers[n] = s_exit_handlers[n + 1];
                --s_exit_handler_count;
                found = true;
                break;
            }
        }
        if (!found)
            return;
        handler.function(handler.argument);
    }
}

// The C library's own setup, before any other initializer (the dynamic
// linker calls it, like every initializer, with the arguments and
// environment).
extern "C" void __libc_initialize(int argc, char** argv, char** envp)
{
    __pthread_initialize();
    environ = envp;
    if (argc > 0 && argv && argv[0])
        setprogname(argv[0]);
}

__attribute__((section(".init_array.00001"), used)) static Constructor s_initialize = __libc_initialize;

// What functions with arrays on the stack check on the way out (-fstack-protector-strong).
extern "C" {
unsigned long __stack_chk_guard = 0x00dead5ca1ab1e00;

[[noreturn]] void __stack_chk_fail()
{
    static char const message[] = "*** stack smashing detected ***: terminated\n";
    (void)System::syscall(System::Syscall::write, 2, reinterpret_cast<unsigned long>(message), sizeof(message) - 1);
    abort();
}
}

// From the 16 random bytes the kernel gives each new program.
[[gnu::no_stack_protector]] static void set_stack_guard(unsigned long* stack)
{
    int const argc = static_cast<int>(stack[0]);
    char** envp = reinterpret_cast<char**>(stack + 1) + argc + 1;
    while (*envp)
        ++envp;
    auto const* auxiliary = reinterpret_cast<unsigned long const*>(envp + 1);
    for (; auxiliary[0] != AT_NULL; auxiliary += 2) {
        if (auxiliary[0] == AT_RANDOM) {
            __stack_chk_guard = *reinterpret_cast<unsigned long const*>(auxiliary[1]) & ~0xfful;
            return;
        }
    }
}

extern "C" [[noreturn, gnu::no_stack_protector]] void __libc_start(unsigned long* stack)
{
    set_stack_guard(stack);
    int argc = static_cast<int>(stack[0]);
    char** argv = reinterpret_cast<char**>(stack + 1);
    char** envp = argv + argc + 1;
    if (!_DYNAMIC) {
        set_up_static_thread_local_storage(stack);
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
    // This thread's thread_local objects; then the exit handlers, in
    // reverse order of registration; then destructors and buffers.
    __libc_run_thread_exit_handlers();
    for (;;) {
        ExitHandler handler;
        {
            LibC::LockGuard guard(s_exit_handlers_lock);
            if (s_exit_handler_count == 0)
                break;
            handler = s_exit_handlers[--s_exit_handler_count];
        }
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

void __exit_fork_lock()
{
    s_exit_handlers_lock.lock();
}

void __exit_fork_unlock(int)
{
    s_exit_handlers_lock.unlock();
}

void __assertion_failed(char const* expression, char const* file, unsigned line, char const* function)
{
    fprintf(stderr, "ASSERTION FAILED: %s\n  at %s:%u in %s()\n", expression, file, line, function);
    abort();
}
}
