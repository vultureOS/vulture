/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibSystem/Syscall.h>

// What the thread pointer points at, and where a thread's static TLS block is from it, as each
// architecture's ELF thread-local storage has it. dyld (or the C library, in a statically linked
// program) sets it up for the first thread; pthread_create copies `image` for each new one.
//
//   AArch64: the pointer is at the start: two reserved words, then the block, the program's module first.
//   x86_64: the pointer is past the end of the block, with the program's module closest to it, and
//           what's at the pointer starts with its own address.

namespace System {

// Every module's initial data, laid out as a thread's block is.
struct ThreadLocalImage {
    void const* data;
    unsigned long size;
    unsigned long alignment;
};

#if defined(__x86_64__)
struct ThreadControlBlock {
    ThreadControlBlock* self;
    ThreadLocalImage const* image;
    void* thread; // The C library's own (pthread_self()).
    unsigned long reserved;
};

static_assert(sizeof(ThreadControlBlock) == 32);

inline ThreadControlBlock* thread_control_block()
{
    unsigned long value;
    asm volatile("movq %%fs:0, %0" : "=r"(value));
    return reinterpret_cast<ThreadControlBlock*>(value);
}

inline void set_thread_control_block(ThreadControlBlock* block)
{
    block->self = block;
    // (A processor register for it that programs may set isn't something all of them have.)
    syscall(Syscall::set_thread_pointer, reinterpret_cast<unsigned long>(block));
}

// What the first module's offset from the pointer is, given what's already there. This is the
// static linker's sum, for the program's: it doesn't have to be told, if it goes first.
inline unsigned long next_tls_offset(unsigned long end, unsigned long virtual_address, unsigned long memory_size, unsigned long alignment)
{
    return end + memory_size + ((0 - virtual_address - end - memory_size) & (alignment - 1));
}

// The offset from the pointer of a module that next_tls_offset() said goes this far down.
inline unsigned long tls_pointer_offset(unsigned long distance)
{
    return 0 - distance;
}

// The room left by every module taken together: `end` is where the last one ended up.
inline unsigned long tls_image_size(unsigned long end, unsigned long alignment)
{
    return (end + alignment - 1) & ~(alignment - 1);
}

// Where in the image a module goes, from its offset from the pointer.
inline unsigned long tls_image_index(unsigned long offset, unsigned long image_size)
{
    return image_size + offset;
}

inline unsigned long thread_storage_size(unsigned long image_size)
{
    return image_size + sizeof(ThreadControlBlock);
}

// Copies the image into storage (of the above size, aligned to the image's alignment), for one thread.
inline ThreadControlBlock* initialize_thread_storage(void* storage, ThreadLocalImage const& image)
{
    auto* bytes = static_cast<unsigned char*>(storage);
    auto const* source = static_cast<unsigned char const*>(image.data);
    for (unsigned long i = 0; i < image.size; ++i)
        bytes[i] = source[i];
    auto* control = reinterpret_cast<ThreadControlBlock*>(bytes + image.size);
    control->self = control;
    control->image = &image;
    control->thread = nullptr;
    control->reserved = 0;
    return control;
}
#else
struct ThreadControlBlock {
    ThreadLocalImage const* image;
    void* thread; // The C library's own (pthread_self()).
};

static_assert(sizeof(ThreadControlBlock) == 16);

inline ThreadControlBlock* thread_control_block()
{
    unsigned long value;
    asm volatile("mrs %0, tpidr_el0" : "=r"(value));
    return reinterpret_cast<ThreadControlBlock*>(value);
}

inline void set_thread_control_block(ThreadControlBlock* block)
{
    asm volatile("msr tpidr_el0, %0" : : "r"(block));
}

// From what the static linker assumes for the program: past the control block's place, then up to an
// address that agrees with the segment's own modulo its alignment. (The return value is the offset,
// and also where the module starts.)
inline unsigned long next_tls_offset(unsigned long end, unsigned long virtual_address, unsigned long, unsigned long alignment)
{
    if (end < sizeof(ThreadControlBlock))
        end = sizeof(ThreadControlBlock);
    return end + ((virtual_address - end) & (alignment - 1));
}

inline unsigned long tls_pointer_offset(unsigned long start)
{
    return start;
}

inline unsigned long tls_image_size(unsigned long end, unsigned long)
{
    return end < sizeof(ThreadControlBlock) ? sizeof(ThreadControlBlock) : end;
}

inline unsigned long tls_image_index(unsigned long offset, unsigned long)
{
    return offset;
}

inline unsigned long thread_storage_size(unsigned long image_size)
{
    return image_size;
}

inline ThreadControlBlock* initialize_thread_storage(void* storage, ThreadLocalImage const& image)
{
    auto* bytes = static_cast<unsigned char*>(storage);
    auto const* source = static_cast<unsigned char const*>(image.data);
    for (unsigned long i = 0; i < image.size; ++i)
        bytes[i] = source[i];
    auto* control = reinterpret_cast<ThreadControlBlock*>(bytes);
    control->image = &image;
    control->thread = nullptr;
    return control;
}
#endif

}
