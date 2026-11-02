/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Optional.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>

// What the firmware's ACPI tables say about the machine: on a PC, this is what a device tree is on Arm.
// Read once, early, from memory the loader says the firmware left them in.

namespace Kernel::ACPI {

struct LocalApic {
    u32 id;
};

struct IoApic {
    u32 id;
    u64 address;
    u32 first_interrupt; // The global system interrupt its first input is.
};

// An ISA interrupt that isn't wired to the input of the same number (the timer usually isn't).
struct InterruptOverride {
    u8 source;
    u32 target;
    bool active_low;
    bool level_triggered;
};

struct Register {
    u8 space { 0 }; // 0: memory; 1: port I/O.
    u64 address { 0 };
};

struct FixedDescription {
    u32 smi_command { 0 };
    u8 acpi_enable { 0 };
    u32 pm1a_control { 0 };
    u32 pm1b_control { 0 };
    Optional<Register> reset_register;
    u8 reset_value { 0 };
    bool has_i8042 { true };
};

struct PciSegment {
    u64 base;
    u16 segment;
    u8 first_bus;
    u8 last_bus;
};

// False if there are no tables to read.
[[nodiscard]] bool initialize(PhysicalAddress rsdp);
[[nodiscard]] bool is_available();

[[nodiscard]] Vector<LocalApic> const& local_apics();
[[nodiscard]] Vector<IoApic> const& io_apics();
[[nodiscard]] Vector<InterruptOverride> const& interrupt_overrides();
[[nodiscard]] u64 local_apic_address();
[[nodiscard]] FixedDescription const& fixed_description();
[[nodiscard]] Vector<PciSegment> const& pci_segments();
// The high precision event timer's registers, if it has one.
[[nodiscard]] Optional<u64> hpet_address();
// The differentiated system description's bytecode, for LibACPI.
[[nodiscard]] ReadonlyBytes differentiated_description();

}
