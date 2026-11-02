/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

// Arm PrimeCell UART (PL011). The firmware has already set the line
// parameters. Output is polled; input comes by interrupt once a receiver
// is attached.
class PL011 {
public:
    void initialize(FlatPtr registers);

    // Points at a new mapping of the same registers.
    void relocate(FlatPtr registers) { m_registers = reinterpret_cast<u32 volatile*>(registers); }

    [[nodiscard]] bool is_present() const { return m_registers != nullptr; }

    void write(char);

    // Calls `receiver(context, byte)` in interrupt context for each byte
    // that arrives.
    using Receiver = void (*)(void* context, u8);
    void enable_receive_interrupt(u32 interrupt, Receiver, void* context);

private:
    void handle_interrupt();

    u32 volatile* m_registers { nullptr };
    Receiver m_receiver { nullptr };
    void* m_receiver_context { nullptr };
};

}
