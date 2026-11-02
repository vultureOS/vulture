/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel::X86_64 {

struct [[gnu::packed]] TaskStateSegment {
    u32 reserved0;
    u64 rsp[3]; // rsp[0]: the stack to enter the kernel on from user mode.
    u64 reserved1;
    u64 ist[7]; // Stacks of their own, for what can't trust the one it finds.
    u64 reserved2;
    u16 reserved3;
    u16 io_bitmap_offset;
};

static_assert(sizeof(TaskStateSegment) == 104);

// What %gs points at in the kernel: one for each processor. Entry.S and CPUInlines.h know these
// offsets; the rest is here to have a home.
struct ProcessorArea {
    void* processor;
    u64 scratch;
    u64 kernel_stack;
    TaskStateSegment* tss_pointer;
    u64 apic_id;
    u64 active_root; // The page table this processor is using.
    u64 gdt[7];
    TaskStateSegment tss;
    alignas(16) u8 nmi_stack[1024];
    alignas(16) u8 double_fault_stack[8192];
};

static_assert(__builtin_offsetof(ProcessorArea, scratch) == 8);
static_assert(__builtin_offsetof(ProcessorArea, kernel_stack) == 16);
static_assert(__builtin_offsetof(ProcessorArea, tss_pointer) == 24);
static_assert(__builtin_offsetof(ProcessorArea, apic_id) == 32);

constexpr u16 selector_kernel_code = 0x08;
constexpr u16 selector_kernel_data = 0x10;
constexpr u16 selector_user_data = 0x18 | 3;
constexpr u16 selector_user_code = 0x20 | 3;
constexpr u16 selector_tss = 0x28;

// The one for this processor, and how to set one up: its segments, interrupts, and all the rest of
// what a processor needs before it can run the kernel.
[[nodiscard]] ProcessorArea& area_at(u32 index);
void initialize_this_processor(ProcessorArea&);

// Vectors. Below this, what the processor raises for itself, and devices above it.
constexpr u32 first_device_vector = 0x30;
constexpr u32 last_device_vector = 0xdf;
constexpr u32 vector_timer = 0xe0;
constexpr u32 vector_ipi_base = 0xf0;
constexpr u32 vector_spurious = 0xff;
constexpr u32 vector_syscall = 0x100;

// Interrupts: acknowledged at the local APIC.
void end_of_interrupt();

}
