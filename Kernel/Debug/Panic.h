/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/TrapFrame.h>
#include <LibBase/StringView.h>

namespace Kernel {

// Stops the system and reports why: the reason, the CPU, the registers (if
// an exception led here) and a symbolized backtrace, on serial and on screen.
[[noreturn]] void panic(StringView reason, TrapFrame const* = nullptr);

// True once a panic has begun. What's written to disk after that (see DiskLog.h) goes the ordinary way through the
// ordinary drivers, with nobody left to wake anybody: locks are not waited for, and drivers watch their devices rather
// than sleep until an interrupt.
[[nodiscard]] bool is_panicking();

// For unattended runs (kernel tests): power off after reporting a panic
// instead of halting, so the machine stops and the failure is seen.
void set_power_off_on_panic(bool);

}
