/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/aarch64/GIC.h>
#include <Kernel/Arch/aarch64/MMIO.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Tasks/Processor.h>

// Arm Generic Interrupt Controller v2, as the GIC-400 of a Raspberry Pi 4 and 5: a distributor shared by all
// the processors, whose first registers each sees its own copy of (for SGIs, used as IPIs, and PPIs, such as
// the timer), and a CPU interface, also its own, at one address. Every interrupt has the same priority, so
// handlers never nest.
//
// Groups are left as they were. Where the firmware has put the kernel in the non-secure state, it can only
// see Group 1 and has already put everything there, and "enable" is that group's; on a GIC without security
// extensions, "enable" is Group 0, where everything starts, and that signals IRQ as well.

namespace Kernel::AArch64::GICv2 {

namespace {

constexpr size_t GICD_CTLR = 0x000;
constexpr size_t GICD_TYPER = 0x004;
constexpr size_t GICD_ISENABLER = 0x100;
constexpr size_t GICD_ICENABLER = 0x180;
constexpr size_t GICD_ICPENDR = 0x280;
constexpr size_t GICD_ICACTIVER = 0x380;
constexpr size_t GICD_IPRIORITYR = 0x400;
constexpr size_t GICD_ITARGETSR = 0x800;
constexpr size_t GICD_ICFGR = 0xc00;
constexpr size_t GICD_SGIR = 0xf00;

constexpr size_t GICC_CTLR = 0x00;
constexpr size_t GICC_PMR = 0x04;
constexpr size_t GICC_BPR = 0x08;
constexpr size_t GICC_IAR = 0x0c;
constexpr size_t GICC_EOIR = 0x10;

constexpr u32 sgir_all_but_self = 1u << 24;
constexpr u32 default_priority = 0xa0;
constexpr u32 priority_word = default_priority * 0x01010101u;
constexpr u32 first_spi = 32;
constexpr u32 first_special_interrupt = 1020;

struct ProcessorInterface {
    u64 hardware_id { 0 };
    u8 mask { 0 };
};

FlatPtr s_distributor = 0;
FlatPtr s_cpu_interface = 0;
u64 s_distributor_physical = 0;
u32 s_interrupt_lines = 0;
ProcessorInterface s_interfaces[Processor::max_count];

// The registers of the first 32 lines are this processor's own, so this says which one it is, as a bit.
u8 this_processors_mask()
{
    return static_cast<u8>(mmio_read32(s_distributor + GICD_ITARGETSR) & 0xff);
}

}

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const& device_tree, DeviceTree::Node const& node)
{
    auto distributor = device_tree.reg(node, 0);
    auto cpu_interface = device_tree.reg(node, 1);
    if (!distributor.has_value() || !cpu_interface.has_value())
        return Error::from_string_literal("the GIC does not say where its registers are");
    s_distributor_physical = distributor->address;
    s_distributor = TRY(memory_manager().map_mmio(PhysicalAddress(distributor->address), distributor->size));
    s_cpu_interface = TRY(memory_manager().map_mmio(PhysicalAddress(cpu_interface->address), cpu_interface->size));

    FlatPtr const gicd = s_distributor;
    mmio_write32(gicd + GICD_CTLR, 0);
    s_interrupt_lines = min<u32>(32 * ((mmio_read32(gicd + GICD_TYPER) & 0x1f) + 1), first_special_interrupt);

    // Every SPI: disabled, not pending, level-triggered, default priority, going to this (the boot) processor.
    u32 const route = this_processors_mask() * 0x01010101u;
    for (u32 interrupt = first_spi; interrupt < s_interrupt_lines; interrupt += 32) {
        size_t word = interrupt / 32 * 4;
        mmio_write32(gicd + GICD_ICENABLER + word, ~0u);
        mmio_write32(gicd + GICD_ICPENDR + word, ~0u);
        mmio_write32(gicd + GICD_ICACTIVER + word, ~0u);
    }
    for (u32 interrupt = first_spi; interrupt < s_interrupt_lines; interrupt += 4) {
        mmio_write32(gicd + GICD_IPRIORITYR + interrupt, priority_word);
        mmio_write32(gicd + GICD_ITARGETSR + interrupt, route);
    }
    for (u32 interrupt = first_spi; interrupt < s_interrupt_lines; interrupt += 16)
        mmio_write32(gicd + GICD_ICFGR + interrupt / 4, 0);
    mmio_write32(gicd + GICD_CTLR, 1);
    return {};
}

ErrorOr<void> initialize_on_this_processor()
{
    VERIFY(s_distributor);
    FlatPtr const gicd = s_distributor;

    // SGIs and PPIs, which are this processor's own: disabled, default priority.
    mmio_write32(gicd + GICD_ICENABLER, ~0u);
    mmio_write32(gicd + GICD_ICPENDR, ~0u);
    mmio_write32(gicd + GICD_ICACTIVER, ~0u);
    for (u32 interrupt = 0; interrupt < first_spi; interrupt += 4)
        mmio_write32(gicd + GICD_IPRIORITYR + interrupt, priority_word);

    auto& entry = s_interfaces[Processor::current().id()];
    entry.hardware_id = Arch::processor_hardware_id();
    entry.mask = this_processors_mask();

    mmio_write32(s_cpu_interface + GICC_PMR, 0xff); // Let every priority through.
    mmio_write32(s_cpu_interface + GICC_BPR, 0);
    mmio_write32(s_cpu_interface + GICC_CTLR, 1);

    // IPIs are SGIs 0 to ipi_count - 1.
    mmio_write32(gicd + GICD_ISENABLER, (1u << ipi_count) - 1);
    return {};
}

void enable_interrupt(u32 interrupt)
{
    VERIFY(interrupt < s_interrupt_lines);
    mmio_write32(s_distributor + GICD_ISENABLER + interrupt / 32 * 4, 1u << (interrupt % 32));
}

void disable_interrupt(u32 interrupt)
{
    VERIFY(interrupt < s_interrupt_lines);
    mmio_write32(s_distributor + GICD_ICENABLER + interrupt / 32 * 4, 1u << (interrupt % 32));
}

void send_ipi(u64 target_hardware_id, IPI ipi)
{
    if (!s_distributor)
        return;
    for (auto const& entry : s_interfaces) {
        if (entry.mask == 0 || entry.hardware_id != target_hardware_id)
            continue;
        // Whatever the target is told to look at must be visible before the IPI.
        asm volatile("dsb ishst" ::: "memory");
        mmio_write32(s_distributor + GICD_SGIR, (static_cast<u32>(entry.mask) << 16) | static_cast<u32>(ipi));
        return;
    }
}

void send_ipi_to_others(IPI ipi)
{
    if (!s_distributor)
        return;
    asm volatile("dsb ishst" ::: "memory");
    mmio_write32(s_distributor + GICD_SGIR, sgir_all_but_self | static_cast<u32>(ipi));
}

void describe(FormatSink& sink)
{
    format_to(sink, "GICv2, distributor at {:#x}, {} interrupt lines", s_distributor_physical, s_interrupt_lines);
}

void handle_irq()
{
    auto& processor = Processor::current();
    processor.enter_interrupt();
    for (;;) {
        u32 acknowledge = mmio_read32(s_cpu_interface + GICC_IAR);
        // Complete the acknowledge before touching the device or memory.
        asm volatile("dsb sy" ::: "memory");
        // (An SGI's says who sent it, above this, and is given back whole.)
        u32 interrupt = acknowledge & 0x3ff;
        if (interrupt >= first_special_interrupt)
            break;
        if (interrupt < ipi_count)
            Processor::handle_ipi(static_cast<IPI>(interrupt));
        else
            dispatch_interrupt(interrupt);
        mmio_write32(s_cpu_interface + GICC_EOIR, acknowledge);
    }
    processor.leave_interrupt();
}

}
