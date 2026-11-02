/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/x86_64/MMIO.h>
#include <Kernel/Devices/Serial/BootSerial.h>
#include <Kernel/Interrupts/InterruptHandlers.h>

namespace Kernel {

namespace {

constexpr u16 reg_data = 0;
constexpr u16 reg_interrupt_enable = 1;
constexpr u16 reg_fifo_control = 2;
constexpr u16 reg_line_control = 3;
constexpr u16 reg_modem_control = 4;
constexpr u16 reg_line_status = 5;
constexpr u16 reg_scratch = 7;

constexpr u8 line_divisor_latch = 1 << 7;
constexpr u8 line_8n1 = 3;
constexpr u8 status_data_ready = 1 << 0;
constexpr u8 status_transmit_empty = 1 << 5;
constexpr u8 interrupt_receive = 1 << 0;
// Data terminal ready, request to send, and the line that lets its interrupt out.
constexpr u8 modem_ready = (1 << 0) | (1 << 1) | (1 << 3);

}

void UART16550::initialize(FlatPtr base)
{
    auto const port = static_cast<u16>(base);
    // A machine without one reads back all ones, whatever is written. (They're on their way out.)
    Arch::port_write8(port + reg_scratch, 0x5a);
    if (Arch::port_read8(port + reg_scratch) != 0x5a)
        return;
    Arch::port_write8(port + reg_interrupt_enable, 0);
    Arch::port_write8(port + reg_line_control, line_divisor_latch);
    Arch::port_write8(port + reg_data, 1);
    Arch::port_write8(port + reg_interrupt_enable, 0);
    Arch::port_write8(port + reg_line_control, line_8n1);
    // First in, first out, both emptied, raising its interrupt at the first byte.
    Arch::port_write8(port + reg_fifo_control, 0x07);
    Arch::port_write8(port + reg_modem_control, modem_ready);
    m_base = port;
}

void UART16550::write(char character)
{
    if (!m_base)
        return;
    while (!(Arch::port_read8(m_base + reg_line_status) & status_transmit_empty)) {
    }
    Arch::port_write8(m_base + reg_data, static_cast<u8>(character));
}

void UART16550::enable_receive_interrupt(u32 interrupt, Receiver receiver, void* context)
{
    if (!m_base)
        return;
    m_receiver = receiver;
    m_receiver_context = context;
    register_interrupt_handler(interrupt, [](void* serial) { static_cast<UART16550*>(serial)->handle_interrupt(); }, this);
    Arch::port_write8(m_base + reg_interrupt_enable, interrupt_receive);
    Arch::enable_interrupt(interrupt);
}

void UART16550::handle_interrupt()
{
    while (Arch::port_read8(m_base + reg_line_status) & status_data_ready) {
        u8 const byte = Arch::port_read8(m_base + reg_data);
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
