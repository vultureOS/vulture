/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Console/Framebuffer.h>

// The quiet boot screen: the vultureOS logo on black with a progress bar
// below it. Booting with "-v" shows the kernel log instead.

namespace Kernel::BootScreen {

void show(Framebuffer&);

// `percent` from 0 to 100.
void set_progress(Framebuffer&, u32 percent);

}
