/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

constexpr u32 max_interrupts = 1024;

// Runs in interrupt context, with interrupts masked, on the processor that
// took the interrupt.
using InterruptHandler = void (*)(void* context);

void register_interrupt_handler(u32 interrupt, InterruptHandler, void* context);

// Called by the interrupt controller driver for each device interrupt.
void dispatch_interrupt(u32 interrupt);

}
