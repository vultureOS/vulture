/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/TrapFrame.h>

namespace Kernel {

// Runs the system call a program asked for (SVC) and stores the result in
// its saved registers.
void handle_syscall(TrapFrame&);

}
