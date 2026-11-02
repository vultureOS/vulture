/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

// The serial port every PC has had since the 1980s, at an I/O port rather than an address. Output is
// polled; input comes by interrupt once a receiver is attached. The same interface as PL011's.
class UART16550 {
public:
    // Sets it to 115200 8N1. `base` is the port number; it's not there if it doesn't answer.
    void initialize(FlatPtr base);

    // I/O ports aren't mapped, so there's nothing to do.
    void relocate(FlatPtr) { }

    [[nodiscard]] bool is_present() const { return m_base != 0; }

    void write(char);

    using Receiver = void (*)(void* context, u8);
    void enable_receive_interrupt(u32 interrupt, Receiver, void* context);

private:
    void handle_interrupt();

    u16 m_base { 0 };
    Receiver m_receiver { nullptr };
    void* m_receiver_context { nullptr };
};

}
