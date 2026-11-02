/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(__x86_64__)
#    include <Kernel/Devices/Serial/UART16550.h>
#else
#    include <Kernel/Devices/Serial/PL011.h>
#endif

namespace Kernel {

// The serial port the kernel's log goes to, of whichever kind this architecture has.
#if defined(__x86_64__)
using BootSerial = UART16550;
#else
using BootSerial = PL011;
#endif

BootSerial& boot_serial();

}
