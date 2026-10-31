/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/Timer.h>
#include <Kernel/Arch/aarch64/MMIO.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Time/Time.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel::Time {

static constexpr u64 nanoseconds_per_second = 1'000'000'000;

static u64 s_ticks_per_second = 0;
// Wall-clock time at counter zero, in nanoseconds since 1970.
static u64 s_epoch_at_counter_zero = 0;
static bool s_has_wall_clock = false;

static void handle_timer_interrupt(void*)
{
    Processor::current().timer_queue().handle_interrupt();
}

ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    s_ticks_per_second = Arch::monotonic_counter_frequency();
    if (s_ticks_per_second == 0)
        return Error::from_string_literal("the counter frequency is unknown");
    u32 interrupt = TRY(Arch::initialize_timer(device_tree));
    register_interrupt_handler(interrupt, handle_timer_interrupt, nullptr);
    return {};
}

void initialize_on_this_processor()
{
    Arch::initialize_timer_on_this_processor();
}

u64 ticks_per_second()
{
    return s_ticks_per_second;
}

// Whole seconds and the remainder separately: each product stays below
// 2^64 for any counter up to 18 GHz, with no 128-bit division (which would
// need a compiler runtime routine).
u64 ticks_from_nanoseconds(u64 nanoseconds)
{
    return nanoseconds / nanoseconds_per_second * s_ticks_per_second + nanoseconds % nanoseconds_per_second * s_ticks_per_second / nanoseconds_per_second;
}

u64 nanoseconds_from_ticks(u64 ticks)
{
    return ticks / s_ticks_per_second * nanoseconds_per_second + ticks % s_ticks_per_second * nanoseconds_per_second / s_ticks_per_second;
}

void initialize_wall_clock(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    // The Arm PL031 real-time clock: its data register counts seconds.
    auto node = device_tree.find_compatible_node("arm,pl031"_sv);
    if (!node.has_value()) {
        dmesgln("Clock: no real-time clock; the time counts from 0 at boot");
        return;
    }
    auto reg = device_tree.reg(node.value(), 0);
    if (!reg.has_value()) {
        dmesgln("Clock: the real-time clock has no registers");
        return;
    }
    auto registers = memory_manager().map_mmio(PhysicalAddress(reg->address), reg->size);
    if (registers.is_error()) {
        dmesgln("Clock: cannot map the real-time clock: {}", registers.error());
        return;
    }
    constexpr size_t data_register = 0x00;
    u64 seconds = AArch64::mmio_read32(registers.value() + data_register);
    u64 counter_now = nanoseconds_from_ticks(now());
    s_epoch_at_counter_zero = seconds * nanoseconds_per_second - counter_now;
    s_has_wall_clock = true;
    dmesgln("Clock: PL031 real-time clock reads {} seconds since 1970", seconds);
}

bool has_wall_clock()
{
    return s_has_wall_clock;
}

u64 wall_clock_nanoseconds()
{
    return s_epoch_at_counter_zero + (s_ticks_per_second ? nanoseconds_from_ticks(now()) : 0);
}

i64 wall_clock_seconds()
{
    return static_cast<i64>(wall_clock_nanoseconds() / nanoseconds_per_second);
}

void wait_until(u64 deadline)
{
    VERIFY(Arch::are_interrupts_enabled());
    if (Scheduler::is_running()) {
        Thread::sleep_until(deadline);
        return;
    }
    // Before threads exist: wait right here. There is no periodic tick, so arm a timer to end the wait.
    Timer wakeup;
    auto& queue = Processor::current().timer_queue();
    queue.add(wakeup, deadline, [](Timer&) { });
    while (now() < deadline) {
        // Masked while checking, so a wakeup between the check and the
        // wait is not lost: WFI returns for a pending interrupt even then.
        auto state = Arch::disable_interrupts();
        if (now() < deadline)
            Arch::wait_for_interrupt();
        Arch::restore_interrupts(state);
    }
    wakeup.cancel();
}

}
