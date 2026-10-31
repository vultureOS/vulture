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

// For unattended runs (kernel tests): power off after reporting a panic
// instead of halting, so the machine stops and the failure is seen.
void set_power_off_on_panic(bool);

}
