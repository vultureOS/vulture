/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/Timer.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Time/Time.h>

namespace Kernel::Time {

static constexpr u64 nanoseconds_per_second = 1'000'000'000;

static u64 s_ticks_per_second = 0;

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

void wait_until(u64 deadline)
{
    VERIFY(Arch::are_interrupts_enabled());
    // There is no periodic tick, so arm a timer to end the wait.
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
    queue.cancel(wakeup);
}

}
