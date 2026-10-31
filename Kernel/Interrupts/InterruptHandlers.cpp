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

// PCI INTx lines are shared, so an interrupt may have a few handlers; each
// checks whether its device asked.
constexpr size_t max_handlers_per_interrupt = 4;

struct Registration {
    InterruptHandler handler;
    void* context;
};

}

// Written before the interrupt is enabled, read only in interrupt context.
static Registration s_handlers[max_interrupts][max_handlers_per_interrupt];

void register_interrupt_handler(u32 interrupt, InterruptHandler handler, void* context)
{
    VERIFY(interrupt < max_interrupts);
    for (auto& slot : s_handlers[interrupt]) {
        if (!slot.handler) {
            slot = { handler, context };
            return;
        }
    }
    VERIFY_NOT_REACHED();
}

void dispatch_interrupt(u32 interrupt)
{
    if (interrupt < max_interrupts && s_handlers[interrupt][0].handler) {
        for (auto& slot : s_handlers[interrupt]) {
            if (slot.handler)
                slot.handler(slot.context);
        }
        return;
    }
    dmesgln("Interrupts: no handler for interrupt {}", interrupt);
}

}
