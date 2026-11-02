/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/SMP.h>
#include <Kernel/Arch/aarch64/PSCI.h>
#include <Kernel/Arch/aarch64/SystemRegisters.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// A secondary processor starts in secondary_entry (Boot.S) with its MMU
// off, so it runs at a physical address. To turn the MMU on underneath
// itself, that code needs an identity mapping (virtual = physical) in the
// lower half; it then jumps to its kernel address and drops the mapping.

extern "C" u8 secondary_entry[];
extern "C" u8 spin_table_entry[];

// Where each processor released from a spin table finds its StartupParameters, by the low byte of its MPIDR.
// Read with the MMU off.
extern "C" {
u64 spin_table_parameters[256] = { 0 };
}

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
    u64 pointer_key_low;
    u64 pointer_key_high;
    u64 has_pointer_authentication;
};

static_assert(sizeof(StartupParameters) == 96);

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

// Where the device tree says this processor is waiting to be told where to go, if it does: firmware
// without PSCI parks them, each watching an address of its own.
static Optional<u64> spin_table_release_address(u64 hardware_id)
{
    auto tree = DeviceTree::FlattenedDeviceTree::parse(memory_manager().device_tree());
    if (tree.is_error())
        return {};
    Optional<u64> release;
    bool in_cpus = false;
    auto result = tree.value().for_each_node([&](DeviceTree::Node const& node) {
        if (node.depth == 1) {
            if (in_cpus)
                return DeviceTree::IterationDecision::Break;
            in_cpus = node.name == "cpus"_sv;
            return DeviceTree::IterationDecision::Continue;
        }
        if (!in_cpus || node.depth != 2)
            return DeviceTree::IterationDecision::Continue;
        auto reg = tree.value().reg(node, 0);
        if (!reg.has_value() || reg->address != hardware_id)
            return DeviceTree::IterationDecision::Continue;
        auto method = tree.value().find_property(node, "enable-method"_sv);
        auto address = tree.value().find_property(node, "cpu-release-addr"_sv);
        if (method.has_value() && method->as_string() == "spin-table"_sv && address.has_value())
            release = address->read_cells(0, 2);
        return DeviceTree::IterationDecision::Break;
    });
    if (result.is_error())
        return {};
    return release;
}

static ErrorOr<void> release_from_spin_table(u64 hardware_id, u64 release_address, PhysicalAddress parameters)
{
    constexpr u64 number_mask = 0xff;
    auto& entry = spin_table_parameters[hardware_id & number_mask];
    entry = parameters.get();
    clean_to_point_of_coherency(&entry, sizeof(entry));

    // The address it's watching isn't in the direct map: it belongs to the firmware.
    auto page = TRY(memory_manager().map_mmio(PhysicalAddress(align_down<u64>(release_address, page_size)), page_size));
    auto* watched = reinterpret_cast<u64 volatile*>(page + (release_address & (page_size - 1)));
    *watched = memory_manager().physical_address_of_kernel_image(spin_table_entry).get();
    asm volatile("dsb sy\n"
                 "sev" ::: "memory");
    return {};
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
    parameters.has_pointer_authentication = has_pointer_authentication();
    if (parameters.has_pointer_authentication) {
        // All the processors sign the kernel's return addresses with the one key.
        parameters.pointer_key_low = READ_SYSTEM_REGISTER(S3_0_C2_C1_2);
        parameters.pointer_key_high = READ_SYSTEM_REGISTER(S3_0_C2_C1_3);
    }
    clean_to_point_of_coherency(&parameters, sizeof(parameters));

    if (auto release_address = spin_table_release_address(hardware_id); release_address.has_value())
        return release_from_spin_table(hardware_id, release_address.value(), slot_address);
    auto entry_physical = memory_manager().physical_address_of_kernel_image(secondary_entry);
    return AArch64::PSCI::cpu_on(hardware_id, entry_physical.get(), slot_address.get());
}

size_t list_processors(DeviceTree::FlattenedDeviceTree const& device_tree, u64* hardware_ids, size_t capacity)
{
    // In /cpus, in device tree order.
    size_t count = 0;
    bool in_cpus = false;
    auto result = device_tree.for_each_node([&](DeviceTree::Node const& node) {
        if (node.depth == 1) {
            if (in_cpus)
                return DeviceTree::IterationDecision::Break;
            in_cpus = node.name == "cpus"_sv;
            return DeviceTree::IterationDecision::Continue;
        }
        if (!in_cpus || node.depth != 2)
            return DeviceTree::IterationDecision::Continue;
        auto device_type = device_tree.find_property(node, "device_type"_sv);
        if (!device_type.has_value() || device_type->as_string() != "cpu"_sv)
            return DeviceTree::IterationDecision::Continue;
        auto reg = device_tree.reg(node, 0);
        if (reg.has_value() && count < capacity)
            hardware_ids[count++] = reg->address;
        return DeviceTree::IterationDecision::Continue;
    });
    if (result.is_error())
        dmesgln("SMP: unreadable device tree ({})", result.error());
    return count;
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
