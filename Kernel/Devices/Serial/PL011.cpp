/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Devices/Serial/PL011.h>

namespace Kernel {

// Register offsets, in 32-bit words.
static constexpr size_t data_register = 0x00 / 4;
static constexpr size_t flag_register = 0x18 / 4;
static constexpr size_t interrupt_mask_register = 0x38 / 4;

static constexpr u32 transmit_fifo_full = 1 << 5;

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
    while (m_registers[flag_register] & transmit_fifo_full) {
    }
    m_registers[data_register] = static_cast<u8>(character);
}

PL011& boot_serial()
{
    static PL011 serial;
    return serial;
}

}
