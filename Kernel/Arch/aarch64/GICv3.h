/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Kernel::AArch64 {

// From the IRQ exception vector: acknowledges, dispatches and completes
// every pending interrupt.
void handle_irq();

}
