/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Kernel {

// Once there is randomness. Whatever calls this must not itself be protected, as its frame was
// set up with the old value.
void initialize_stack_protector();

}
