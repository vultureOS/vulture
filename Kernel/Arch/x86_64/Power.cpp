/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/Arch/x86_64/MMIO.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Firmware/ACPI.h>
#include <Kernel/Time/Time.h>
#include <LibACPI/AML.h>

namespace Kernel::Arch {

namespace {

constexpr u16 control_sleep_enable = 1 << 13;
constexpr u16 control_sci_enable = 1 << 0;

Optional<::ACPI::SleepType> s_sleep_type;

// Nothing has a say over this once it starts, and a machine that won't reset has to be told something.
[[noreturn]] void give_up()
{
    dmesgln("Power management: the machine is still running");
    halt();
}

}

ErrorOr<void> initialize_power_management(DeviceTree::FlattenedDeviceTree const&)
{
    if (!ACPI::is_available())
        return Error::from_string_literal("no ACPI tables");
    s_sleep_type = ::ACPI::find_sleep_type_s5(ACPI::differentiated_description());
    if (!s_sleep_type.has_value())
        dmesgln("Power management: the firmware's tables don't say how to power off");
    // Some machines start with power management in the firmware's hands, until asked for it.
    auto const& fixed = ACPI::fixed_description();
    if (fixed.pm1a_control != 0 && !(port_read16(static_cast<u16>(fixed.pm1a_control)) & control_sci_enable)
        && fixed.smi_command != 0 && fixed.acpi_enable != 0) {
        port_write8(static_cast<u16>(fixed.smi_command), fixed.acpi_enable);
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(300);
        while (!(port_read16(static_cast<u16>(fixed.pm1a_control)) & control_sci_enable) && Time::now() < deadline)
            pause();
    }
    return {};
}

// The clock on the motherboard, with its battery: in decimal digits, unless a register says otherwise.
Optional<u64> read_real_time_clock(DeviceTree::FlattenedDeviceTree const&)
{
    constexpr u16 port_index = 0x70;
    constexpr u16 port_data = 0x71;
    auto read = [](u8 reg) {
        port_write8(port_index, reg);
        return port_read8(port_data);
    };
    // Not while it's in the middle of changing.
    for (int i = 0; i < 1'000'000 && (read(0x0a) & 0x80); ++i)
        pause();
    u8 const format = read(0x0b);
    bool const binary = format & 0x04;
    bool const twenty_four_hours = format & 0x02;
    auto value = [&](u8 raw) -> u32 { return binary ? raw : (raw >> 4) * 10 + (raw & 0xf); };
    u32 const second = value(read(0x00));
    u32 const minute = value(read(0x02));
    u8 const raw_hour = read(0x04);
    u32 hour = value(raw_hour & 0x7f);
    if (!twenty_four_hours) {
        hour %= 12;
        if (raw_hour & 0x80)
            hour += 12;
    }
    u32 const day = value(read(0x07));
    u32 const month = value(read(0x08));
    u32 const year = 2000 + value(read(0x09));
    if (month < 1 || month > 12 || day < 1)
        return {};
    // Days since 1970, by way of the year starting in March, so that leap days come last.
    i64 const y = month <= 2 ? static_cast<i64>(year) - 1 : year;
    i64 const era = y / 400;
    i64 const year_of_era = y - era * 400;
    i64 const shifted_month = month > 2 ? month - 3 : month + 9;
    i64 const day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    i64 const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    i64 const days = era * 146097 + day_of_era - 719468;
    return static_cast<u64>(days * 86400 + hour * 3600 + minute * 60 + second);
}

void describe_power_management(FormatSink& sink)
{
    if (!ACPI::is_available()) {
        sink.write("none"_sv);
        return;
    }
    format_to(sink, "ACPI, {} off", s_sleep_type.has_value() ? "soft"_sv : "no soft");
}

void power_off()
{
    auto const& fixed = ACPI::fixed_description();
    if (s_sleep_type.has_value() && fixed.pm1a_control != 0) {
        port_write16(static_cast<u16>(fixed.pm1a_control), static_cast<u16>((s_sleep_type->a << 10) | control_sleep_enable));
        if (fixed.pm1b_control != 0)
            port_write16(static_cast<u16>(fixed.pm1b_control), static_cast<u16>((s_sleep_type->b << 10) | control_sleep_enable));
        // It takes a moment.
        for (int i = 0; i < 100'000'000; ++i)
            pause();
    }
    give_up();
}

void reboot()
{
    auto const& fixed = ACPI::fixed_description();
    if (fixed.reset_register.has_value()) {
        auto const& reset = fixed.reset_register.value();
        if (reset.space == 1)
            port_write8(static_cast<u16>(reset.address), fixed.reset_value);
        for (int i = 0; i < 10'000'000; ++i)
            pause();
    }
    // The chipset's own reset register.
    port_write8(0xcf9, 0x02);
    port_write8(0xcf9, 0x06);
    for (int i = 0; i < 10'000'000; ++i)
        pause();
    // The keyboard controller's reset line: what started out as a way out of protected mode.
    port_write8(0x64, 0xfe);
    for (int i = 0; i < 10'000'000; ++i)
        pause();
    give_up();
}

}
