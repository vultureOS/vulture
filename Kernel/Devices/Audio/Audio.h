/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Kernel {

// Finds sound devices, and makes /dev/audio0 of the first (see <Kernel/API/POSIX/audio.h>).
void initialize_audio();

}
