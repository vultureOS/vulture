/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
#include <Kernel/Arch/PageTable.h>
#include <Kernel/Arch/SMP.h>
#include <Kernel/Arch/x86_64/APIC.h>
#include <Kernel/Arch/x86_64/ProcessorArea.h>
#include <Kernel/Firmware/ACPI.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// A secondary processor starts in real mode, at the start of a page below 1 MiB, and has to get from
// there to the kernel by way of protected mode and long mode. The loader kept some pages down there
// for this; Boot.S has the code. Each of those steps has tables it can use: at first three pages next to
// the code, as the page table register is 32 bits wide until long mode, and then a copy of the kernel's
// that also has the code's page, as that's what's running while it changes over.

extern "C" u8 x86_64_trampoline_start[];
extern "C" u8 x86_64_trampoline_parameters[];
extern "C" u8 x86_64_trampoline_end[];

namespace Kernel::Arch {

namespace {

// What the trampoline reads, in its own copy (Boot.S has these offsets), and then what's used in C++.
struct StartupParameters {
    u64 first_tables;
    u64 second_tables;
    u64 stack_top;
    u64 entry;
    u64 argument_entry;
    u64 argument;
    u64 kernel_tables;
    u64 area_index;
};
static_assert(sizeof(StartupParameters) == 64);

constexpr u64 table_present_writable = 3;
constexpr u64 table_large = 1 << 7;

PhysicalAddress s_low_memory;
PageTable s_identity_table;
bool s_prepared = false;

// Where a secondary processor finishes its journey, on its own stack, in the kernel at last.
extern "C" [[noreturn, gnu::no_stack_protector]] void x86_64_secondary_start(StartupParameters const* low_parameters)
{
    StartupParameters const parameters = *low_parameters;
    X86_64::initialize_this_processor(X86_64::area_at(static_cast<u32>(parameters.area_index)));
    X86_64::area_at(static_cast<u32>(parameters.area_index)).active_root = parameters.kernel_tables;
    asm volatile("movq %0, %%cr3" ::"r"(parameters.kernel_tables) : "memory");
    reinterpret_cast<SecondaryEntry>(parameters.argument_entry)(reinterpret_cast<void*>(parameters.argument));
    for (;;)
        asm volatile("hlt");
}

ErrorOr<void> prepare()
{
    if (s_low_memory.get() == 0)
        return Error::from_string_literal("the loader kept no low memory to start processors in");
    auto* low = physical_to_virtual(s_low_memory);
    size_t const code_size = static_cast<size_t>(x86_64_trampoline_end - x86_64_trampoline_start);
    VERIFY(code_size <= page_size);
    __builtin_memcpy(low, x86_64_trampoline_start, code_size);

    // The next three pages: 2 MiB, mapped to itself.
    u64 const base = s_low_memory.get();
    auto* level0 = reinterpret_cast<u64*>(low + page_size);
    auto* level1 = reinterpret_cast<u64*>(low + 2 * page_size);
    auto* level2 = reinterpret_cast<u64*>(low + 3 * page_size);
    __builtin_memset(level0, 0, 3 * page_size);
    level0[0] = (base + 2 * page_size) | table_present_writable;
    level1[0] = (base + 3 * page_size) | table_present_writable;
    level2[0] = 0 | table_present_writable | table_large;

    s_identity_table = TRY(PageTable::create());
    TRY(s_identity_table.map(base, s_low_memory, page_size, MemoryType::Normal, Protection::ReadExecute));
    s_prepared = true;
    return {};
}

}

void set_low_memory(PhysicalAddress address)
{
    s_low_memory = address;
}

ErrorOr<void> start_processor(u64 hardware_id, FlatPtr stack_top, SecondaryEntry entry, void* argument)
{
    if (!s_prepared)
        TRY(prepare());
    size_t const offset = static_cast<size_t>(x86_64_trampoline_parameters - x86_64_trampoline_start);
    auto& parameters = *reinterpret_cast<StartupParameters*>(physical_to_virtual(s_low_memory) + offset);
    parameters.first_tables = s_low_memory.get() + page_size;
    parameters.second_tables = s_identity_table.root().get();
    parameters.stack_top = stack_top;
    parameters.entry = reinterpret_cast<u64>(x86_64_secondary_start);
    parameters.argument_entry = reinterpret_cast<u64>(entry);
    parameters.argument = reinterpret_cast<u64>(argument);
    parameters.kernel_tables = memory_manager().kernel_page_table().root().get();
    // (It has just been registered, as the last of them.)
    parameters.area_index = Processor::count() - 1;

    X86_64::send_init_ipi(static_cast<u32>(hardware_id));
    // Twice, as the specification has it: the second is ignored if the first worked.
    X86_64::send_startup_ipi(static_cast<u32>(hardware_id), s_low_memory.get());
    X86_64::send_startup_ipi(static_cast<u32>(hardware_id), s_low_memory.get());
    return {};
}

void finish_starting_processors()
{
    if (!s_prepared)
        return;
    s_identity_table.destroy();
    s_prepared = false;
}

size_t list_processors(DeviceTree::FlattenedDeviceTree const&, u64* hardware_ids, size_t capacity)
{
    size_t count = 0;
    for (auto const& apic : ACPI::local_apics()) {
        if (count == capacity)
            break;
        hardware_ids[count++] = apic.id;
    }
    return count;
}

}
