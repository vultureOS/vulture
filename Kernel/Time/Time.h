/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/CPU.h>
#include <LibBase/Error.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

// Time in ticks of the monotonic counter, and conversions to real units.

namespace Kernel::Time {

// Finds the timer hardware and routes its interrupt to the timer queues.
ErrorOr<void> initialize(DeviceTree::FlattenedDeviceTree const&);
void initialize_on_this_processor();

[[nodiscard]] inline u64 now() { return Arch::monotonic_counter(); }
[[nodiscard]] u64 ticks_per_second();

[[nodiscard]] u64 ticks_from_nanoseconds(u64);
[[nodiscard]] u64 nanoseconds_from_ticks(u64);
[[nodiscard]] inline u64 ticks_from_microseconds(u64 microseconds) { return ticks_from_nanoseconds(microseconds * 1000); }
[[nodiscard]] inline u64 ticks_from_milliseconds(u64 milliseconds) { return ticks_from_nanoseconds(milliseconds * 1'000'000); }
[[nodiscard]] inline u64 microseconds_from_ticks(u64 ticks) { return nanoseconds_from_ticks(ticks) / 1000; }

// Reads the real-time clock (if the device tree has one), so wall-clock
// time can be told from then on.
void initialize_wall_clock(DeviceTree::FlattenedDeviceTree const&);
// Seconds since 1970 (UTC); counts from 0 at boot without a clock.
[[nodiscard]] i64 wall_clock_seconds();
[[nodiscard]] u64 wall_clock_nanoseconds();
[[nodiscard]] bool has_wall_clock();

// Sleeps until the counter reaches `deadline`: the current thread sleeps
// (before the scheduler starts, the processor waits). Interrupts must be
// enabled.
void wait_until(u64 deadline);

}
