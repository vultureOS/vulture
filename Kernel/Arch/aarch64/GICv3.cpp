/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/aarch64/GIC.h>
#include <Kernel/Arch/aarch64/MMIO.h>
#include <Kernel/Arch/aarch64/SystemRegisters.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Tasks/Processor.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// Arm Generic Interrupt Controller v3: a distributor for shared peripheral
// interrupts (SPIs), a redistributor per processor for its private ones
// (SGIs, used as IPIs, and PPIs, such as the timer), and a CPU interface
// reached through system registers. Every interrupt is Group 1 (IRQ) at
// the same priority, so handlers never nest.

namespace Kernel::AArch64::GICv3 {

using namespace Kernel::Arch;

namespace {

// Distributor registers.
constexpr size_t GICD_CTLR = 0x0000;
constexpr size_t GICD_TYPER = 0x0004;
constexpr size_t GICD_IGROUPR = 0x0080;
constexpr size_t GICD_ISENABLER = 0x0100;
constexpr size_t GICD_ICENABLER = 0x0180;
constexpr size_t GICD_ICPENDR = 0x0280;
constexpr size_t GICD_ICACTIVER = 0x0380;
constexpr size_t GICD_IPRIORITYR = 0x0400;
constexpr size_t GICD_ICFGR = 0x0c00;
constexpr size_t GICD_IGRPMODR = 0x0d00;
constexpr size_t GICD_IROUTER = 0x6000;

constexpr u32 GICD_CTLR_RWP = 1u << 31;
constexpr u32 GICD_CTLR_DS = 1u << 6;
constexpr u32 GICD_CTLR_ARE = 1u << 4;
// EnableGrp1 with a single security state, EnableGrp1A in the non-secure view.
constexpr u32 GICD_CTLR_ENABLE_GROUP1 = 1u << 1;
// EnableGrp1 in the non-secure view of a GIC with two security states.
constexpr u32 GICD_CTLR_ENABLE_GROUP1_NONSECURE = 1u << 0;

// Redistributor: a control frame, then the SGI/PPI frame 64 KiB above it.
constexpr size_t GICR_CTLR = 0x0000;
constexpr size_t GICR_TYPER = 0x0008;
constexpr size_t GICR_WAKER = 0x0014;
constexpr size_t GICR_SGI_FRAME = 0x10000;
constexpr size_t GICR_IGROUPR0 = GICR_SGI_FRAME + 0x0080;
constexpr size_t GICR_ISENABLER0 = GICR_SGI_FRAME + 0x0100;
constexpr size_t GICR_ICENABLER0 = GICR_SGI_FRAME + 0x0180;
constexpr size_t GICR_ICPENDR0 = GICR_SGI_FRAME + 0x0280;
constexpr size_t GICR_ICACTIVER0 = GICR_SGI_FRAME + 0x0380;
constexpr size_t GICR_IPRIORITYR = GICR_SGI_FRAME + 0x0400;
constexpr size_t GICR_ICFGR1 = GICR_SGI_FRAME + 0x0c04;
constexpr size_t GICR_IGRPMODR0 = GICR_SGI_FRAME + 0x0d00;

constexpr u32 GICR_CTLR_RWP = 1u << 3;
constexpr u64 GICR_TYPER_VLPIS = 1ull << 1;
constexpr u64 GICR_TYPER_LAST = 1ull << 4;
constexpr u32 GICR_WAKER_PROCESSOR_SLEEP = 1u << 1;
constexpr u32 GICR_WAKER_CHILDREN_ASLEEP = 1u << 2;

constexpr u32 default_priority = 0xa0;
constexpr u32 priority_word = default_priority * 0x01010101u;
constexpr u32 first_spi = 32;
constexpr u32 first_special_interrupt = 1020;
constexpr size_t max_redistributor_regions = 4;
constexpr u32 register_wait_spins = 1'000'000;

struct RedistributorRegion {
    FlatPtr base { 0 };
    u64 size { 0 };
};

struct GIC {
    FlatPtr distributor { 0 };
    u64 distributor_physical { 0 };
    RedistributorRegion regions[max_redistributor_regions];
    size_t region_count { 0 };
    u64 stride { 0 }; // 0: derived from each redistributor's GICR_TYPER.
    u32 interrupt_lines { 0 }; // The highest SPI plus one.
    bool single_security_state { false };
    FlatPtr redistributors[Processor::max_count] {};
};

GIC s_gic;

}

static ErrorOr<void> wait_for_register_write(FlatPtr address, u32 pending_bit)
{
    for (u32 spin = 0; spin < register_wait_spins; ++spin) {
        if (!(mmio_read32(address) & pending_bit))
            return {};
        Arch::pause();
    }
    return Error::from_string_literal("the GIC did not finish a register write");
}

static ErrorOr<void> wait_for_distributor()
{
    return wait_for_register_write(s_gic.distributor + GICD_CTLR, GICD_CTLR_RWP);
}

static FlatPtr current_redistributor()
{
    FlatPtr redistributor = s_gic.redistributors[Processor::current().id()];
    VERIFY(redistributor);
    return redistributor;
}

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const& device_tree, DeviceTree::Node const& node_in_tree)
{
    Optional<DeviceTree::Node> node = node_in_tree;

    auto distributor = device_tree.reg(node.value(), 0);
    if (!distributor.has_value())
        return Error::from_string_literal("the GIC has no distributor registers");
    s_gic.distributor_physical = distributor->address;
    s_gic.distributor = TRY(memory_manager().map_mmio(PhysicalAddress(distributor->address), distributor->size));

    u32 region_count = 1;
    if (auto property = device_tree.find_property(node.value(), "#redistributor-regions"_sv); property.has_value())
        region_count = property->as_u32().value_or(1);
    for (u32 i = 0; i < region_count && s_gic.region_count < max_redistributor_regions; ++i) {
        auto region = device_tree.reg(node.value(), 1 + i);
        if (!region.has_value())
            return Error::from_string_literal("the GIC is missing a redistributor region");
        auto base = TRY(memory_manager().map_mmio(PhysicalAddress(region->address), region->size));
        s_gic.regions[s_gic.region_count++] = { base, region->size };
    }
    if (auto property = device_tree.find_property(node.value(), "redistributor-stride"_sv); property.has_value())
        s_gic.stride = property->read_cells(0, 2).value_or(0);

    FlatPtr gicd = s_gic.distributor;
    mmio_write32(gicd + GICD_CTLR, 0);
    TRY(wait_for_distributor());
    s_gic.single_security_state = mmio_read32(gicd + GICD_CTLR) & GICD_CTLR_DS;
    s_gic.interrupt_lines = min<u32>(32 * ((mmio_read32(gicd + GICD_TYPER) & 0x1f) + 1), first_special_interrupt);

    // Every SPI: disabled, not pending, Group 1, level-triggered, default
    // priority, routed to this (the boot) processor.
    for (u32 interrupt = first_spi; interrupt < s_gic.interrupt_lines; interrupt += 32) {
        size_t word = interrupt / 32 * 4;
        mmio_write32(gicd + GICD_ICENABLER + word, ~0u);
        mmio_write32(gicd + GICD_ICPENDR + word, ~0u);
        mmio_write32(gicd + GICD_ICACTIVER + word, ~0u);
        mmio_write32(gicd + GICD_IGROUPR + word, ~0u);
        mmio_write32(gicd + GICD_IGRPMODR + word, 0);
    }
    for (u32 interrupt = first_spi; interrupt < s_gic.interrupt_lines; interrupt += 4)
        mmio_write32(gicd + GICD_IPRIORITYR + interrupt, priority_word);
    for (u32 interrupt = first_spi; interrupt < s_gic.interrupt_lines; interrupt += 16)
        mmio_write32(gicd + GICD_ICFGR + interrupt / 4, 0);
    TRY(wait_for_distributor());

    // Affinity routing must be on before the groups are enabled.
    mmio_write32(gicd + GICD_CTLR, GICD_CTLR_ARE);
    TRY(wait_for_distributor());
    u32 enable = GICD_CTLR_ENABLE_GROUP1 | (s_gic.single_security_state ? 0 : GICD_CTLR_ENABLE_GROUP1_NONSECURE);
    mmio_write32(gicd + GICD_CTLR, GICD_CTLR_ARE | enable);
    TRY(wait_for_distributor());

    // GICD_IROUTER takes the affinity fields where MPIDR keeps them.
    u64 route = processor_hardware_id();
    for (u32 interrupt = first_spi; interrupt < s_gic.interrupt_lines; ++interrupt)
        mmio_write64(gicd + GICD_IROUTER + 8 * interrupt, route);
    return {};
}

// The redistributor whose GICR_TYPER names this processor's affinity.
static FlatPtr find_redistributor(u64 hardware_id)
{
    u64 wanted = (((hardware_id >> 32) & 0xff) << 24) | (hardware_id & 0xff'ffff);
    for (size_t i = 0; i < s_gic.region_count; ++i) {
        auto const& region = s_gic.regions[i];
        FlatPtr frame = region.base;
        while (frame < region.base + region.size) {
            u64 typer = mmio_read64(frame + GICR_TYPER);
            if ((typer >> 32) == wanted)
                return frame;
            if (typer & GICR_TYPER_LAST)
                break;
            frame += s_gic.stride ? s_gic.stride : ((typer & GICR_TYPER_VLPIS) ? 0x40000 : 0x20000);
        }
    }
    return 0;
}

ErrorOr<void> initialize_on_this_processor()
{
    VERIFY(s_gic.distributor);
    FlatPtr gicr = find_redistributor(processor_hardware_id());
    if (!gicr)
        return Error::from_string_literal("no GIC redistributor for this processor");
    s_gic.redistributors[Processor::current().id()] = gicr;

    mmio_write32(gicr + GICR_WAKER, mmio_read32(gicr + GICR_WAKER) & ~GICR_WAKER_PROCESSOR_SLEEP);
    TRY(wait_for_register_write(gicr + GICR_WAKER, GICR_WAKER_CHILDREN_ASLEEP));

    // SGIs and PPIs: disabled, Group 1, default priority, PPIs level-triggered.
    mmio_write32(gicr + GICR_ICENABLER0, ~0u);
    mmio_write32(gicr + GICR_ICPENDR0, ~0u);
    mmio_write32(gicr + GICR_ICACTIVER0, ~0u);
    mmio_write32(gicr + GICR_IGROUPR0, ~0u);
    mmio_write32(gicr + GICR_IGRPMODR0, 0);
    for (u32 interrupt = 0; interrupt < 32; interrupt += 4)
        mmio_write32(gicr + GICR_IPRIORITYR + interrupt, priority_word);
    mmio_write32(gicr + GICR_ICFGR1, 0);
    TRY(wait_for_register_write(gicr + GICR_CTLR, GICR_CTLR_RWP));

    // The CPU interface, through system registers.
    WRITE_SYSTEM_REGISTER(icc_sre_el1, READ_SYSTEM_REGISTER(icc_sre_el1) | 1);
    AArch64::instruction_barrier();
    if (!(READ_SYSTEM_REGISTER(icc_sre_el1) & 1))
        return Error::from_string_literal("the GIC system register interface is not available");
    WRITE_SYSTEM_REGISTER(icc_pmr_el1, 0xff); // Let every priority through.
    WRITE_SYSTEM_REGISTER(icc_bpr1_el1, 0);
    // EOImode 0: writing EOIR both drops the priority and deactivates.
    WRITE_SYSTEM_REGISTER(icc_ctlr_el1, READ_SYSTEM_REGISTER(icc_ctlr_el1) & ~(1ull << 1));
    WRITE_SYSTEM_REGISTER(icc_igrpen1_el1, 1);
    AArch64::instruction_barrier();

    // IPIs are SGIs 0 to ipi_count - 1.
    mmio_write32(gicr + GICR_ISENABLER0, (1u << ipi_count) - 1);
    return {};
}

void enable_interrupt(u32 interrupt)
{
    if (interrupt < first_spi) {
        mmio_write32(current_redistributor() + GICR_ISENABLER0, 1u << interrupt);
        return;
    }
    VERIFY(interrupt < s_gic.interrupt_lines);
    mmio_write32(s_gic.distributor + GICD_ISENABLER + interrupt / 32 * 4, 1u << (interrupt % 32));
}

void disable_interrupt(u32 interrupt)
{
    if (interrupt < first_spi) {
        FlatPtr gicr = current_redistributor();
        mmio_write32(gicr + GICR_ICENABLER0, 1u << interrupt);
        (void)wait_for_register_write(gicr + GICR_CTLR, GICR_CTLR_RWP);
        return;
    }
    VERIFY(interrupt < s_gic.interrupt_lines);
    mmio_write32(s_gic.distributor + GICD_ICENABLER + interrupt / 32 * 4, 1u << (interrupt % 32));
    (void)wait_for_distributor();
}

static void write_sgi_register(u64 value)
{
    // Whatever the target is told to look at must be visible before the IPI.
    asm volatile("dsb ishst" ::: "memory");
    WRITE_SYSTEM_REGISTER(icc_sgi1r_el1, value);
    AArch64::instruction_barrier();
}

void send_ipi(u64 target_hardware_id, IPI ipi)
{
    if (!s_gic.distributor)
        return;
    u64 aff0 = target_hardware_id & 0xff;
    u64 aff1 = (target_hardware_id >> 8) & 0xff;
    u64 aff2 = (target_hardware_id >> 16) & 0xff;
    u64 aff3 = (target_hardware_id >> 32) & 0xff;
    // The target list covers 16 Aff0 values; the range selector picks which 16.
    u64 value = (aff3 << 48) | ((aff0 / 16) << 44) | (aff2 << 32) | (static_cast<u64>(ipi) << 24) | (aff1 << 16) | (1ull << (aff0 % 16));
    write_sgi_register(value);
}

void send_ipi_to_others(IPI ipi)
{
    if (!s_gic.distributor)
        return;
    constexpr u64 all_but_self = 1ull << 40;
    write_sgi_register(all_but_self | (static_cast<u64>(ipi) << 24));
}

void describe(FormatSink& sink)
{
    format_to(sink, "GICv3, distributor at {:#x}, {} interrupt lines, {} redistributor region{}, {} security state{}",
        s_gic.distributor_physical, s_gic.interrupt_lines, s_gic.region_count, s_gic.region_count == 1 ? ""_sv : "s"_sv,
        s_gic.single_security_state ? "one"_sv : "two"_sv, s_gic.single_security_state ? ""_sv : "s"_sv);
}

void handle_irq()
{
    auto& processor = Processor::current();
    processor.enter_interrupt();
    for (;;) {
        u64 acknowledge = READ_SYSTEM_REGISTER(icc_iar1_el1);
        // Complete the acknowledge before touching the device or memory.
        asm volatile("dsb sy" ::: "memory");
        u32 interrupt = acknowledge & 0xff'ffff;
        if (interrupt >= first_special_interrupt && interrupt <= 1023)
            break;
        if (interrupt < ipi_count)
            Processor::handle_ipi(static_cast<IPI>(interrupt));
        else
            dispatch_interrupt(interrupt);
        WRITE_SYSTEM_REGISTER(icc_eoir1_el1, acknowledge);
        instruction_barrier();
    }
    processor.leave_interrupt();
}

}
