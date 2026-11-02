/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// What comes after the stub in kernel8.img: this, on a page of its own, then the kernel and the boot archive, each
// on a page boundary. Offsets are from the start of this header. mkpiimage writes it; the stub reads it.

namespace Boot::RaspberryPi {

struct PayloadHeader {
    char magic[8];
    u64 kernel_offset;
    u64 kernel_size;
    u64 archive_offset;
    u64 archive_size;
};

constexpr char payload_magic[8] = { 'V', 'U', 'L', 'T', 'U', 'R', 'E', '1' };

}
