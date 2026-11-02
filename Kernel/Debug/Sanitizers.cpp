/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if VULTURE_KERNEL_SANITIZERS

#    include <Kernel/Debug/KernelLog.h>
#    include <Kernel/Debug/Panic.h>
#    include <Kernel/Memory/Heap.h>
#    include <LibBase/Format.h>

// What a kernel built with the sanitizers calls: for undefined behavior (UBSan), and before every
// load and store (KASAN), with the heap keeping the shadow memory in the checks (see Heap.cpp).
// There is no carrying on after any of these: the first one panics.

namespace {

using namespace Kernel;

struct SourceLocation {
    char const* file;
    u32 line;
    u32 column;
};

size_t length_of(char const* text)
{
    size_t length = 0;
    while (text[length])
        ++length;
    return length;
}

[[noreturn]] void report_undefined(char const* what, void const* data)
{
    auto const* location = static_cast<SourceLocation const*>(data);
    StringView file { location->file, length_of(location->file) };
    if (auto kernel = file.find("/vulture/"_sv); kernel.has_value())
        file = file.substring_view(kernel.value() + 9);
    FixedStringBuilder<192> reason;
    format_to(reason, "UBSan: {} at {}:{}:{}", StringView { what, length_of(what) }, file, location->line, location->column);
    panic(reason.view());
}

[[noreturn]] void report_memory(FlatPtr address, size_t size, bool is_write, StringView problem)
{
    FixedStringBuilder<128> reason;
    format_to(reason, "KASAN: {}: {} of {} bytes at {:#x}", problem, is_write ? "write"_sv : "read"_sv, size, address);
    panic(reason.view());
}

inline void check(FlatPtr address, size_t size, bool is_write)
{
    StringView problem;
    if (size > 0 && !heap_check_access(address, size, problem))
        report_memory(address, size, is_write, problem);
}

}

extern "C" {

#    define UBSAN_HANDLER(name, what)                       \
        [[noreturn]] void __ubsan_handle_##name(void* data) \
        {                                                   \
            report_undefined(what, data);                   \
        }

UBSAN_HANDLER(add_overflow, "signed addition overflow")
UBSAN_HANDLER(sub_overflow, "signed subtraction overflow")
UBSAN_HANDLER(mul_overflow, "signed multiplication overflow")
UBSAN_HANDLER(negate_overflow, "negation overflow")
UBSAN_HANDLER(divrem_overflow, "division overflow or division by zero")
UBSAN_HANDLER(shift_out_of_bounds, "a shift out of bounds")
UBSAN_HANDLER(out_of_bounds, "an index out of bounds")
UBSAN_HANDLER(type_mismatch_v1, "a null, misaligned or too small pointer")
UBSAN_HANDLER(pointer_overflow, "pointer arithmetic that overflows")
UBSAN_HANDLER(builtin_unreachable, "reaching what should be unreachable")
UBSAN_HANDLER(missing_return, "the end of a function that must return a value")
UBSAN_HANDLER(vla_bound_not_positive, "a variable-length array with no positive size")
UBSAN_HANDLER(float_cast_overflow, "a floating point conversion out of range")
UBSAN_HANDLER(load_invalid_value, "loading an invalid value for its type")
UBSAN_HANDLER(invalid_builtin, "an invalid argument to a builtin")
UBSAN_HANDLER(nonnull_return_v1, "null returned where it may not be")
UBSAN_HANDLER(nullability_return_v1, "null returned where it may not be")
UBSAN_HANDLER(nonnull_arg, "null passed where it may not be")
UBSAN_HANDLER(nullability_arg, "null passed where it may not be")
UBSAN_HANDLER(alignment_assumption, "a misaligned pointer that was assumed to be aligned")
UBSAN_HANDLER(implicit_conversion, "an implicit conversion that changes the value")

#    define KASAN_ACCESS(size)                                                 \
        void __asan_load##size##_noabort(FlatPtr address) { check(address, size, false); } \
        void __asan_store##size##_noabort(FlatPtr address) { check(address, size, true); }

KASAN_ACCESS(1)
KASAN_ACCESS(2)
KASAN_ACCESS(4)
KASAN_ACCESS(8)
KASAN_ACCESS(16)

void __asan_loadN_noabort(FlatPtr address, size_t size)
{
    check(address, size, false);
}

void __asan_storeN_noabort(FlatPtr address, size_t size)
{
    check(address, size, true);
}

void __asan_handle_no_return()
{
}

// These stand for memcpy and the rest in code that is checked, and do the checking for them. What
// actually copies is in LibBase, which isn't.
void* __asan_memcpy(void* destination, void const* source, size_t count)
{
    check(reinterpret_cast<FlatPtr>(destination), count, true);
    check(reinterpret_cast<FlatPtr>(source), count, false);
    return __builtin_memcpy(destination, source, count);
}

void* __asan_memmove(void* destination, void const* source, size_t count)
{
    check(reinterpret_cast<FlatPtr>(destination), count, true);
    check(reinterpret_cast<FlatPtr>(source), count, false);
    return __builtin_memmove(destination, source, count);
}

void* __asan_memset(void* destination, int value, size_t count)
{
    check(reinterpret_cast<FlatPtr>(destination), count, true);
    return __builtin_memset(destination, value, count);
}
}

#endif
