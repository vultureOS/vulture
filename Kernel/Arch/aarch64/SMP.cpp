/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/SMP.h>
#include <Kernel/Arch/aarch64/PSCI.h>
#include <Kernel/Arch/aarch64/SystemRegisters.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>

// A secondary processor starts in secondary_entry (Boot.S) with its MMU
// off, so it runs at a physical address. To turn the MMU on underneath
// itself, that code needs an identity mapping (virtual = physical) in the
// lower half; it then jumps to its kernel address and drops the mapping.

extern "C" u8 secondary_entry[];

namespace Kernel::Arch {

namespace {

// Read by secondary_entry with the MMU off; the offsets are fixed there.
struct StartupParameters {
    u64 mair;
    u64 tcr;
    u64 ttbr0;
    u64 ttbr1;
    u64 sctlr;
    u64 vbar;
    u64 stack_top;
    u64 entry;
    u64 argument;
};

static_assert(sizeof(StartupParameters) == 72);

constexpr unsigned parameters_order = 1;
constexpr size_t parameter_slots = (page_size << parameters_order) / sizeof(StartupParameters);
static_assert(parameter_slots >= Processor::max_count);

constexpr u64 tcr_epd0 = 1ull << 7;

PageTable s_identity_table;
PhysicalAddress s_parameters_block;
size_t s_next_slot = 0;

}

static ErrorOr<void> prepare()
{
    s_identity_table = TRY(PageTable::create());
    u64 entry_page = align_down<u64>(memory_manager().physical_address_of_kernel_image(secondary_entry).get(), page_size);
    // Two pages, in case the code crosses a page boundary.
    TRY(s_identity_table.map(entry_page, PhysicalAddress(entry_page), 2 * page_size, MemoryType::Normal, Protection::ReadExecute));
    s_parameters_block = TRY(physical_memory().allocate_pages(parameters_order, PageUse::BootData));
    return {};
}

// The secondary reads its parameters with caches off, straight from memory.
static void clean_to_point_of_coherency(void const* start, size_t size)
{
    u64 line = 4ull << ((READ_SYSTEM_REGISTER(ctr_el0) >> 16) & 0xf);
    auto address = align_down<FlatPtr>(reinterpret_cast<FlatPtr>(start), line);
    for (; address < reinterpret_cast<FlatPtr>(start) + size; address += line)
        asm volatile("dc cvac, %0" ::"r"(address) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

ErrorOr<void> start_processor(u64 hardware_id, FlatPtr stack_top, SecondaryEntry entry, void* argument)
{
    if (s_parameters_block == PhysicalAddress())
        TRY(prepare());
    if (s_next_slot == parameter_slots)
        return Error::from_string_literal("too many processors");

    // Each processor gets its own slot, so a slow starter can never read
    // parameters meant for the next one.
    auto slot_address = s_parameters_block.offset(s_next_slot++ * sizeof(StartupParameters));
    auto& parameters = *physical_to_virtual<StartupParameters>(slot_address);
    parameters.mair = READ_SYSTEM_REGISTER(mair_el1);
    parameters.tcr = READ_SYSTEM_REGISTER(tcr_el1) & ~tcr_epd0;
    parameters.ttbr0 = s_identity_table.root().get();
    parameters.ttbr1 = memory_manager().kernel_page_table().root().get();
    parameters.sctlr = READ_SYSTEM_REGISTER(sctlr_el1);
    parameters.vbar = READ_SYSTEM_REGISTER(vbar_el1);
    parameters.stack_top = stack_top;
    parameters.entry = reinterpret_cast<u64>(entry);
    parameters.argument = reinterpret_cast<u64>(argument);
    clean_to_point_of_coherency(&parameters, sizeof(parameters));

    auto entry_physical = memory_manager().physical_address_of_kernel_image(secondary_entry);
    return AArch64::PSCI::cpu_on(hardware_id, entry_physical.get(), slot_address.get());
}

void finish_starting_processors()
{
    if (s_parameters_block == PhysicalAddress())
        return;
    s_identity_table.destroy();
    physical_memory().free_pages(s_parameters_block);
    s_parameters_block = {};
    s_next_slot = 0;
}

}
