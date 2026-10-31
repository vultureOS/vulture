/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>

// Arm's Power State Coordination Interface: firmware calls to start and
// stop processors and to power the machine off.

namespace Kernel::AArch64::PSCI {

[[nodiscard]] bool is_available();

// Starts the processor with affinity `target` at the physical address
// `entry`, with the MMU off and x0 = `context`.
ErrorOr<void> cpu_on(u64 target, u64 entry, u64 context);

}
