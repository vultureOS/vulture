/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

// Arm PrimeCell UART (PL011), output only. The firmware has already set the
// line parameters; this driver only writes characters.
class PL011 {
public:
    void initialize(FlatPtr registers);

    // Points at a new mapping of the same registers.
    void relocate(FlatPtr registers) { m_registers = reinterpret_cast<u32 volatile*>(registers); }

    [[nodiscard]] bool is_present() const { return m_registers != nullptr; }

    void write(char);

private:
    u32 volatile* m_registers { nullptr };
};

PL011& boot_serial();

}
