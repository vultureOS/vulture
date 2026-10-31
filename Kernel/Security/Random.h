/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>

namespace Kernel {

// The kernel's random numbers: ChaCha20 keyed from the firmware's entropy
// (EFI_RNG_PROTOCOL) and the timer, rekeyed after every request so that
// earlier output cannot be reconstructed from the state ("fast key
// erasure"). For /dev/random, getrandom() and the kernel itself.
void initialize_random(ReadonlyBytes firmware_entropy);
void get_random_bytes(Bytes);
// More unpredictability (interrupt timings and the like).
void add_entropy(u64);

}
