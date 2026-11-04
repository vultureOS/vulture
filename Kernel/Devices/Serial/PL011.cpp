/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Devices/Serial/BootSerial.h>
#include <Kernel/Interrupts/InterruptHandlers.h>

namespace Kernel {

// Register offsets, in 32-bit words.
static constexpr size_t data_register = 0x00 / 4;
static constexpr size_t flag_register = 0x18 / 4;
static constexpr size_t interrupt_mask_register = 0x38 / 4;
static constexpr size_t interrupt_clear_register = 0x44 / 4;

static constexpr u32 receive_fifo_empty = 1 << 4;
static constexpr u32 transmit_fifo_full = 1 << 5;
// Receive, and receive timeout (a few bytes waiting, no more coming).
static constexpr u32 receive_interrupts = (1 << 4) | (1 << 6);

void PL011::initialize(FlatPtr registers)
{
    m_registers = reinterpret_cast<u32 volatile*>(registers);
    // Output is polled; keep the UART from raising interrupts.
    m_registers[interrupt_mask_register] = 0;
}

void PL011::write(char character)
{
    if (!m_registers)
        return;
    // On some machines nobody turned it on, and what's sent stays in its queue. Better no output than no boot.
    constexpr u32 longest_wait = 1'000'000;
    u32 waited = 0;
    while (m_registers[flag_register] & transmit_fifo_full) {
        if (++waited == longest_wait) {
            m_registers = nullptr;
            return;
        }
    }
    m_registers[data_register] = static_cast<u8>(character);
}

void PL011::enable_receive_interrupt(u32 interrupt, Receiver receiver, void* context)
{
    if (!m_registers)
        return;
    m_receiver = receiver;
    m_receiver_context = context;
    register_interrupt_handler(interrupt, [](void* serial) { static_cast<PL011*>(serial)->handle_interrupt(); }, this);
    m_registers[interrupt_clear_register] = 0x7ff;
    m_registers[interrupt_mask_register] = receive_interrupts;
    Arch::enable_interrupt(interrupt);
}

void PL011::handle_interrupt()
{
    // Cleared first: what arrives after this raises it again, whereas clearing it after looking would lose whatever came in between,
    // until something else came along behind it.
    m_registers[interrupt_clear_register] = receive_interrupts;
    while (!(m_registers[flag_register] & receive_fifo_empty)) {
        u8 byte = static_cast<u8>(m_registers[data_register]);
        if (m_receiver)
            m_receiver(m_receiver_context, byte);
    }
}

BootSerial& boot_serial()
{
    static BootSerial serial;
    return serial;
}

}
