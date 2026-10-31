/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <LibBase/Assertions.h>

namespace Kernel {

namespace {

struct Registration {
    InterruptHandler handler;
    void* context;
};

}

// Written before the interrupt is enabled, read only in interrupt context.
static Registration s_handlers[max_interrupts];

void register_interrupt_handler(u32 interrupt, InterruptHandler handler, void* context)
{
    VERIFY(interrupt < max_interrupts);
    VERIFY(!s_handlers[interrupt].handler);
    s_handlers[interrupt] = { handler, context };
}

void dispatch_interrupt(u32 interrupt)
{
    if (interrupt < max_interrupts && s_handlers[interrupt].handler) {
        s_handlers[interrupt].handler(s_handlers[interrupt].context);
        return;
    }
    dmesgln("Interrupts: no handler for interrupt {}", interrupt);
}

}
