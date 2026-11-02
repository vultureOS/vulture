/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/Timer.h>
#include <Kernel/Arch/x86_64/APIC.h>
#include <Kernel/Arch/x86_64/MMIO.h>
#include <Kernel/Arch/x86_64/ProcessorArea.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Firmware/ACPI.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>

// The local APIC, one for each processor: its interrupts, its timer, and sending interrupts to the
// others. It's either at an address (xAPIC) or in model-specific registers (x2APIC), which every recent
// processor has; the firmware may have left it in either mode, and x2APIC can't be left.

namespace Kernel::X86_64 {

namespace {

constexpr u32 msr_apic_base = 0x1b;
constexpr u32 msr_x2apic_base = 0x800;
constexpr u64 apic_base_enable = 1 << 11;
constexpr u64 apic_base_x2apic = 1 << 10;

// Registers, by their offset in xAPIC mode. In x2APIC mode they're at 0x800 + offset / 16.
constexpr u32 reg_task_priority = 0x80;
constexpr u32 reg_eoi = 0xb0;
constexpr u32 reg_spurious = 0xf0;
constexpr u32 reg_error_status = 0x280;
constexpr u32 reg_icr_low = 0x300;
constexpr u32 reg_icr_high = 0x310;
constexpr u32 reg_lvt_timer = 0x320;
constexpr u32 reg_lvt_lint0 = 0x350;
constexpr u32 reg_lvt_lint1 = 0x360;
constexpr u32 reg_lvt_error = 0x370;
constexpr u32 reg_timer_initial = 0x380;
constexpr u32 reg_timer_current = 0x390;
constexpr u32 reg_timer_divide = 0x3e0;

constexpr u32 lvt_masked = 1 << 16;
constexpr u32 spurious_enable = 1 << 8;
constexpr u32 timer_divide_by_16 = 0b0011;

constexpr u32 icr_delivery_fixed = 0 << 8;
constexpr u32 icr_delivery_nmi = 4 << 8;
constexpr u32 icr_delivery_init = 5 << 8;
constexpr u32 icr_delivery_startup = 6 << 8;
constexpr u32 icr_level_assert = 1 << 14;
constexpr u32 icr_trigger_level = 1 << 15;
constexpr u32 icr_all_but_self = 3 << 18;
constexpr u32 icr_pending = 1 << 12;

bool s_x2apic = false;
FlatPtr s_registers = 0;
u64 s_timer_ticks_per_second = 0;
u64 s_tsc_per_second = 0;
Atomic<u32> s_next_vector { first_device_vector };

// The I/O APICs: for what can't send messages.
struct IoApicRegisters {
    FlatPtr base;
    u32 first_interrupt;
    u32 inputs;
};
IoApicRegisters s_io_apics[8];
size_t s_io_apic_count = 0;
// Which input of which is behind a vector: 0 is none.
u32 s_vector_input[256];

u32 read_register(u32 reg)
{
    if (s_x2apic)
        return static_cast<u32>(Arch::read_msr(msr_x2apic_base + reg / 16));
    return Arch::mmio_read32(s_registers + reg);
}

void write_register(u32 reg, u32 value)
{
    if (s_x2apic)
        Arch::write_msr(msr_x2apic_base + reg / 16, value);
    else
        Arch::mmio_write32(s_registers + reg, value);
}

void send_command(u32 apic_id, u32 command)
{
    // What the target has been told to look at must be visible first; in x2APIC mode the write
    // doesn't do this by itself.
    asm volatile("mfence" ::: "memory");
    if (s_x2apic) {
        Arch::write_msr(msr_x2apic_base + reg_icr_low / 16, (static_cast<u64>(apic_id) << 32) | command);
        return;
    }
    while (Arch::mmio_read32(s_registers + reg_icr_low) & icr_pending)
        Arch::pause();
    Arch::mmio_write32(s_registers + reg_icr_high, apic_id << 24);
    Arch::mmio_write32(s_registers + reg_icr_low, command);
}

u32 io_apic_read(IoApicRegisters const& apic, u32 reg)
{
    Arch::mmio_write32(apic.base, reg);
    return Arch::mmio_read32(apic.base + 0x10);
}

void io_apic_write(IoApicRegisters const& apic, u32 reg, u32 value)
{
    Arch::mmio_write32(apic.base, reg);
    Arch::mmio_write32(apic.base + 0x10, value);
}

IoApicRegisters const* io_apic_for(u32 global_interrupt)
{
    for (size_t i = 0; i < s_io_apic_count; ++i) {
        auto const& apic = s_io_apics[i];
        if (global_interrupt >= apic.first_interrupt && global_interrupt < apic.first_interrupt + apic.inputs)
            return &apic;
    }
    return nullptr;
}

void mask_legacy_controllers()
{
    // The two 8259s from the original PC. Remapped out of the way of exceptions in case one fires
    // before it notices it's been masked, and then masked.
    constexpr u16 primary = 0x20;
    constexpr u16 secondary = 0xa0;
    Arch::port_write8(primary, 0x11);
    Arch::port_write8(secondary, 0x11);
    Arch::port_write8(primary + 1, 0x20);
    Arch::port_write8(secondary + 1, 0x28);
    Arch::port_write8(primary + 1, 4);
    Arch::port_write8(secondary + 1, 2);
    Arch::port_write8(primary + 1, 1);
    Arch::port_write8(secondary + 1, 1);
    Arch::port_write8(primary + 1, 0xff);
    Arch::port_write8(secondary + 1, 0xff);
}

// How far the counter has gone in this many microseconds, by a clock that doesn't depend on it.
u64 measure_counter(u32 microseconds)
{
    if (auto const hpet = ACPI::hpet_address(); hpet.has_value()) {
        auto const mapped = memory_manager().map_mmio(PhysicalAddress(hpet.value()), page_size);
        if (!mapped.is_error()) {
            FlatPtr const registers = mapped.value();
            // Capabilities: its period, in femtoseconds, in the upper half.
            u64 const femtoseconds = Arch::mmio_read64(registers) >> 32;
            if (femtoseconds != 0 && femtoseconds < 0x05f5'e100) {
                constexpr FlatPtr configuration = 0x10;
                constexpr FlatPtr counter = 0xf0;
                Arch::mmio_write64(registers + configuration, Arch::mmio_read64(registers + configuration) | 1);
                u64 const wanted = static_cast<u64>(microseconds) * 1'000'000'000 / femtoseconds;
                u64 const start_hpet = Arch::mmio_read64(registers + counter);
                u64 const start = Arch::monotonic_counter();
                while (Arch::mmio_read64(registers + counter) - start_hpet < wanted)
                    Arch::pause();
                return Arch::monotonic_counter() - start;
            }
        }
    }
    // The 8254 timer, on the channel that can be read without any interrupts: 1.193182 MHz.
    constexpr u16 control = 0x43;
    constexpr u16 channel2 = 0x42;
    constexpr u16 gate = 0x61;
    constexpr u64 pit_hertz = 1'193'182;
    u64 const ticks = pit_hertz * microseconds / 1'000'000;
    Arch::port_write8(gate, (Arch::port_read8(gate) & ~2) | 1);
    Arch::port_write8(control, 0xb0); // Channel 2, both bytes, one-shot.
    Arch::port_write8(channel2, static_cast<u8>(ticks));
    Arch::port_write8(channel2, static_cast<u8>(ticks >> 8));
    u64 const start = Arch::monotonic_counter();
    Arch::port_write8(gate, Arch::port_read8(gate) & ~1);
    Arch::port_write8(gate, Arch::port_read8(gate) | 1);
    while (!(Arch::port_read8(gate) & 0x20))
        Arch::pause();
    return Arch::monotonic_counter() - start;
}

}

void end_of_interrupt()
{
    write_register(reg_eoi, 0);
}

ErrorOr<u32> allocate_message_vector()
{
    u32 const vector = s_next_vector.fetch_add(1);
    if (vector > last_device_vector)
        return Error::from_string_literal("out of interrupt vectors");
    return vector;
}

u64 message_address()
{
    // To the processor that started first: fixed, physical. (Its APIC ID in the address for xAPIC;
    // an ID over 255 goes in the extended bits.)
    u64 const id = area_at(0).apic_id;
    return 0xfee0'0000 | ((id & 0xff) << 12) | ((id >> 8) << 32);
}

u32 message_data(u32 vector)
{
    return vector;
}

ErrorOr<u32> allocate_isa_vector(u8 isa_interrupt)
{
    u32 global = isa_interrupt;
    bool active_low = false;
    bool level = false;
    for (auto const& entry : ACPI::interrupt_overrides()) {
        if (entry.source == isa_interrupt) {
            global = entry.target;
            active_low = entry.active_low;
            level = entry.level_triggered;
        }
    }
    auto const* apic = io_apic_for(global);
    if (!apic)
        return Error::from_string_literal("no I/O APIC has that interrupt");
    u32 const vector = TRY(allocate_message_vector());
    u32 const input = global - apic->first_interrupt;
    u32 low = vector | lvt_masked;
    if (active_low)
        low |= 1 << 13;
    if (level)
        low |= 1 << 15;
    io_apic_write(*apic, 0x10 + input * 2 + 1, static_cast<u32>(area_at(0).apic_id) << 24);
    io_apic_write(*apic, 0x10 + input * 2, low);
    s_vector_input[vector] = global + 1;
    return vector;
}

void send_init_ipi(u32 apic_id)
{
    send_command(apic_id, icr_delivery_init | icr_level_assert | icr_trigger_level);
    Time::wait_until(Time::now() + Time::ticks_from_milliseconds(10));
    if (!s_x2apic)
        send_command(apic_id, icr_delivery_init | icr_trigger_level);
}

void send_startup_ipi(u32 apic_id, u64 page)
{
    send_command(apic_id, icr_delivery_startup | icr_level_assert | static_cast<u32>(page >> 12));
    Time::wait_until(Time::now() + Time::ticks_from_microseconds(200));
}

}

namespace Kernel::Arch {

using namespace X86_64;

ErrorOr<void> initialize_interrupt_controller(DeviceTree::FlattenedDeviceTree const&)
{
    mask_legacy_controllers();
    u64 const base = read_msr(msr_apic_base);
    s_x2apic = (cpuid(1).ecx & (1 << 21)) || (base & apic_base_x2apic);
    if (!s_x2apic) {
        u64 const address = ACPI::is_available() ? ACPI::local_apic_address() : (base & 0xf'ffff'f000);
        s_registers = TRY(memory_manager().map_mmio(PhysicalAddress(address), page_size));
    }
    for (auto const& apic : ACPI::io_apics()) {
        if (s_io_apic_count == sizeof(s_io_apics) / sizeof(s_io_apics[0]))
            break;
        auto& entry = s_io_apics[s_io_apic_count++];
        entry.base = TRY(memory_manager().map_mmio(PhysicalAddress(apic.address), page_size));
        entry.first_interrupt = apic.first_interrupt;
        entry.inputs = ((io_apic_read(entry, 1) >> 16) & 0xff) + 1;
        for (u32 input = 0; input < entry.inputs; ++input)
            io_apic_write(entry, 0x10 + input * 2, lvt_masked);
    }
    return initialize_interrupt_controller_on_this_processor();
}

ErrorOr<void> initialize_interrupt_controller_on_this_processor()
{
    u64 base = read_msr(msr_apic_base) | apic_base_enable;
    if (s_x2apic)
        base |= apic_base_x2apic;
    write_msr(msr_apic_base, base);

    // The ID is what's in the firmware's tables. In x2APIC mode this can be read directly; the area
    // already has it from the processor's own idea of it, which is the same number.
    write_register(reg_task_priority, 0);
    write_register(reg_lvt_lint0, lvt_masked);
    write_register(reg_lvt_lint1, lvt_masked);
    write_register(reg_lvt_error, vector_spurious);
    write_register(reg_error_status, 0);
    write_register(reg_error_status, 0);
    write_register(reg_spurious, spurious_enable | vector_spurious);
    end_of_interrupt();
    return {};
}

void enable_interrupt(u32 interrupt)
{
    // Message interrupts and the local APIC's own have nothing to switch on.
    if (interrupt >= 256 || s_vector_input[interrupt] == 0)
        return;
    u32 const global = s_vector_input[interrupt] - 1;
    auto const* apic = io_apic_for(global);
    u32 const reg = 0x10 + (global - apic->first_interrupt) * 2;
    io_apic_write(*apic, reg, io_apic_read(*apic, reg) & ~lvt_masked);
}

void disable_interrupt(u32 interrupt)
{
    if (interrupt >= 256 || s_vector_input[interrupt] == 0)
        return;
    u32 const global = s_vector_input[interrupt] - 1;
    auto const* apic = io_apic_for(global);
    u32 const reg = 0x10 + (global - apic->first_interrupt) * 2;
    io_apic_write(*apic, reg, io_apic_read(*apic, reg) | lvt_masked);
}

static u32 ipi_command(IPI ipi)
{
    // Stopping has to get through to a processor spinning with interrupts masked.
    if (ipi == IPI::Stop)
        return icr_delivery_nmi | icr_level_assert;
    return icr_delivery_fixed | icr_level_assert | (vector_ipi_base + static_cast<u32>(ipi));
}

void send_ipi(u64 target_hardware_id, IPI ipi)
{
    send_command(static_cast<u32>(target_hardware_id), ipi_command(ipi));
}

void send_ipi_to_others(IPI ipi)
{
    send_command(0, ipi_command(ipi) | icr_all_but_self);
}

void describe_interrupt_controller(FormatSink& sink)
{
    format_to(sink, "{}, {} I/O APIC{}", s_x2apic ? "x2APIC"_sv : "xAPIC"_sv, s_io_apic_count, s_io_apic_count == 1 ? ""_sv : "s"_sv);
}

u64 monotonic_counter_frequency()
{
    if (s_tsc_per_second == 0) {
        constexpr u32 microseconds = 20'000;
        s_tsc_per_second = measure_counter(microseconds) * (1'000'000 / microseconds);
    }
    return s_tsc_per_second;
}

ErrorOr<u32> initialize_timer(DeviceTree::FlattenedDeviceTree const&)
{
    // How fast it counts, against the counter, which by now is measured.
    write_register(reg_timer_divide, timer_divide_by_16);
    write_register(reg_lvt_timer, lvt_masked | vector_timer);
    write_register(reg_timer_initial, 0xffff'ffff);
    u64 const start = monotonic_counter();
    u64 const wait = s_tsc_per_second / 100;
    while (monotonic_counter() - start < wait)
        pause();
    u32 const counted = 0xffff'ffff - read_register(reg_timer_current);
    write_register(reg_timer_initial, 0);
    s_timer_ticks_per_second = static_cast<u64>(counted) * 100;
    if (s_timer_ticks_per_second == 0)
        return Error::from_string_literal("the local APIC's timer doesn't count");
    return vector_timer;
}

void initialize_timer_on_this_processor()
{
    write_register(reg_timer_divide, timer_divide_by_16);
    write_register(reg_timer_initial, 0);
    // One-shot, unmasked.
    write_register(reg_lvt_timer, vector_timer);
}

void set_timer_deadline(u64 deadline)
{
    u64 const now = monotonic_counter();
    if (deadline <= now) {
        write_register(reg_timer_initial, 1);
        return;
    }
    u64 const remaining = deadline - now;
    // Whole seconds and the remainder separately, so that this stays within 64 bits.
    u64 const count = remaining / s_tsc_per_second * s_timer_ticks_per_second + remaining % s_tsc_per_second * s_timer_ticks_per_second / s_tsc_per_second;
    // A deadline further away than it can count just fires early, and gets set again.
    write_register(reg_timer_initial, static_cast<u32>(clamp<u64>(count, 1, 0xffff'ffff)));
}

void stop_timer()
{
    write_register(reg_timer_initial, 0);
}

}
