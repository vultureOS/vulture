/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Kernel {

// Finds keyboards and pointers, and makes /dev/input.
void initialize_input();

// From a driver's thread: an event. To whoever has /dev/input open; if nobody
// does, keys type at the console.
void post_input_event(u16 type, u16 code, i32 value);

}
