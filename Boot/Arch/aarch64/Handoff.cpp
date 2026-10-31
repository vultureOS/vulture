/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Arch/aarch64/Handoff.h>
#include <Kernel/Arch/aarch64/PageTables.h>

namespace Boot::AArch64 {

static constexpr u64 sctlr_mmu_enable = 1ull << 0;
static constexpr u64 sctlr_data_cache_enable = 1ull << 2;
static constexpr u64 sctlr_instruction_cache_enable = 1ull << 12;

#define READ_SYSTEM_REGISTER(name)                     \
    ({                                                 \
        u64 value_;                                    \
        asm volatile("mrs %0, " #name : "=r"(value_)); \
        value_;                                        \
    })

ErrorOr<void> check_cpu()
{
    u64 exception_level = (READ_SYSTEM_REGISTER(CurrentEL) >> 2) & 3;
    if (exception_level != 1)
        return Error::from_string_literal("must start at EL1; starting at EL2 is not supported yet");

    u64 granule_4k = (READ_SYSTEM_REGISTER(id_aa64mmfr0_el1) >> 28) & 0xf;
    if (granule_4k == 0xf)
        return Error::from_string_literal("the CPU does not support 4 KiB translation granules");
    return {};
}

u64 physical_address_size()
{
    return READ_SYSTEM_REGISTER(id_aa64mmfr0_el1) & 0xf;
}

HandoffState make_handoff_state(u64 ttbr0, u64 ttbr1, u64 entry, u64 boot_info)
{
    HandoffState state {};
    state.mair = Kernel::AArch64::mair_value;
    state.tcr = Kernel::AArch64::tcr_value(physical_address_size());
    state.ttbr0 = ttbr0;
    state.ttbr1 = ttbr1;
    state.sctlr = READ_SYSTEM_REGISTER(sctlr_el1) | sctlr_mmu_enable | sctlr_data_cache_enable | sctlr_instruction_cache_enable;
    state.entry = entry;
    state.boot_info = boot_info;
    return state;
}

static u64 data_cache_line_size()
{
    return 4ull << ((READ_SYSTEM_REGISTER(ctr_el0) >> 16) & 0xf);
}

void clean_data_cache(u64 start, u64 size)
{
    u64 line = data_cache_line_size();
    for (u64 address = align_down(start, line); address < start + size; address += line)
        asm volatile("dc cvac, %0" ::"r"(address) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

void synchronize_instruction_cache(u64 start, u64 size)
{
    u64 line = data_cache_line_size();
    for (u64 address = align_down(start, line); address < start + size; address += line)
        asm volatile("dc cvau, %0" ::"r"(address) : "memory");
    asm volatile("dsb ish\n"
                 "ic iallu\n"
                 "dsb ish\n"
                 "isb" ::
                     : "memory");
}

// Runs from its identity-mapped physical address. Between turning the MMU
// off and back on it touches registers only: with the MMU off, data accesses
// would bypass the cache.
extern "C" [[gnu::naked]] void vulture_handoff_trampoline(HandoffState const*)
{
    asm volatile(
        "msr daifset, #0xf\n"
        "ldp x1, x2, [x0, #0]\n" // mair, tcr
        "ldp x3, x4, [x0, #16]\n" // ttbr0, ttbr1
        "ldp x5, x6, [x0, #32]\n" // sctlr, entry
        "ldr x7, [x0, #48]\n" // boot_info
        "mrs x8, sctlr_el1\n"
        "bic x8, x8, #(1 << 0)\n" // MMU off
        "bic x8, x8, #(1 << 2)\n" // data cache off
        "msr sctlr_el1, x8\n"
        "isb\n"
        "msr mair_el1, x1\n"
        "msr tcr_el1, x2\n"
        "msr ttbr0_el1, x3\n"
        "msr ttbr1_el1, x4\n"
        "tlbi vmalle1\n"
        "dsb nsh\n"
        "isb\n"
        "msr sctlr_el1, x5\n" // MMU and caches on, with the new tables
        "isb\n"
        "mov x0, x7\n"
        "br x6\n");
}

u64 trampoline_address()
{
    return reinterpret_cast<u64>(&vulture_handoff_trampoline);
}

void enter_kernel(HandoffState const& state)
{
    vulture_handoff_trampoline(&state);
    __builtin_unreachable();
}

}
