/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Kernel {

// Finds display adapters: each adds itself to /dev as gpuN.
void initialize_graphics();

}
