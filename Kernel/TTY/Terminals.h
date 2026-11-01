/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/TTY/TTY.h>

namespace Kernel {

// /dev/console (the screen, and the serial port's input) and /dev/ptmx
// (each open makes a pseudo-terminal: its master, and /dev/ttysNNN).
ErrorOr<void> create_terminals();

[[nodiscard]] TTY& console_tty();
// Something typed at the console (safe from any context).
void queue_console_input(u8);

}
