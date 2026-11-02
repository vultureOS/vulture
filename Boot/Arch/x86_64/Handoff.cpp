/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Arch/x86_64/Handoff.h>

// The loader is a Windows-flavored program (it's what UEFI wants), so its first arguments are in rcx,
// rdx and r8; the kernel is System V, with its first in rdi. This has nothing of its own in memory, and
// nothing on a stack that may be about to disappear.
asm(R"(
.text
.balign 16
.global x86_64_enter_kernel
x86_64_enter_kernel:
    movq %rcx, %cr3
    movq %r8, %rdi
    jmpq *%rdx
)");


extern "C" [[noreturn]] void x86_64_enter_kernel_function(u64 tables, u64 entry, u64 boot_info) asm("x86_64_enter_kernel");

namespace Boot::X86_64 {

namespace {

constexpr u32 msr_efer = 0xc000'0080;
constexpr u64 efer_no_execute = 1 << 11;

u32 extended_features()
{
    u32 eax = 0x8000'0000, ebx, ecx = 0, edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (eax < 0x8000'0001)
        return 0;
    eax = 0x8000'0001;
    ecx = 0;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return edx;
}

}

ErrorOr<void> check_cpu()
{
    constexpr u32 feature_no_execute = 1u << 20;
    if (!(extended_features() & feature_no_execute))
        return Error::from_string_literal("the CPU does not support no-execute pages");
    return {};
}

u64 trampoline_address()
{
    return reinterpret_cast<u64>(&x86_64_enter_kernel_function);
}

void enter_kernel(HandoffState const& state)
{
    asm volatile("cli");
    // The new tables use no-execute pages, and that bit is an error until this is on.
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr_efer));
    low |= efer_no_execute;
    asm volatile("wrmsr" ::"a"(low), "d"(high), "c"(msr_efer));
    x86_64_enter_kernel_function(state.tables, state.entry, state.boot_info);
}

}
