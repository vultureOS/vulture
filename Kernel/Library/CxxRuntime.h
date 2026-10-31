/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Kernel {

// Runs the constructors of global objects (the .init_array section).
void run_global_constructors();

}
