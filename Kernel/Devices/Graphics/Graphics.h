/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Kernel {

// Finds display adapters: each adds itself to /dev as gpuN.
void initialize_graphics();
// That whatever's showing the display goes by the corner of the cursor's picture, and not its hot spot.
void set_display_ignores_cursor_hot_spot(bool);

}
